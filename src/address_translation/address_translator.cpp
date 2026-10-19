#include "address_translation/address_translator.h"

#include <iostream>
#include <stdexcept>

namespace pimid {

AddressTranslator::AddressTranslator(const AddressTranslationConfig& config)
    : config_(config), page_table_(kPageTablePages),
      next_physical_page_(2 * kPageTablePages) {
    if (config_.page_size_bytes != (1u << kPageBits)) {
        throw std::invalid_argument(
            "AddressTranslator currently supports only 4 KiB pages");
    }
    config_.page_bits = kPageBits;

    if (config_.tlb_entries == 0) {
        std::cerr << "WARNING: TLB entries is 0, setting to default 64" << std::endl;
        config_.tlb_entries = 64;
    }

    if (config_.tlb_associativity == 0 ||
        config_.tlb_associativity > config_.tlb_entries ||
        config_.tlb_entries % config_.tlb_associativity != 0) {
        std::cerr << "WARNING: Invalid TLB associativity, setting to fully associative"
                  << std::endl;
        config_.tlb_associativity = config_.tlb_entries;
    }

    std::cout << "AddressTranslator initialized:\n"
              << "  Page size: 4096 bytes\n"
              << "  Page table: 3-level radix, 48-bit VA, 12-bit indices\n"
              << "  TLB entries: " << config_.tlb_entries << "\n"
              << "  TLB associativity: " << config_.tlb_associativity << "\n"
              << "  TLB lookup latency: " << config_.tlb_lookup_latency << " cycles\n"
              << "  PTE reads and writes: PE memory-interface traffic" << std::endl;
}

Address AddressTranslator::translate(Address virtual_addr, uint32_t pe_id,
                                     bool& hit, Cycle& latency, bool is_write,
                                     const PageTableAccess& page_table_access) {
    if (!isCanonicalAddress(virtual_addr)) {
        throw std::out_of_range("AddressTranslator received a non-canonical 48-bit VA");
    }
    if (!page_table_access) {
        throw std::invalid_argument("Page-table walk requires a memory-access callback");
    }

    const Address virtual_page = getPageNumber(virtual_addr);
    const Address page_offset = getPageOffset(virtual_addr);
    latency = config_.tlb_lookup_latency;

    Address physical_page = 0;
    bool hit_needs_pte_update = false;
    Address hit_pte_address = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ensurePE(pe_id);
        const bool collect_stats = stats_enabled_.load();
        ++whole_program_pe_stats_[pe_id].total_translations;
        if (collect_stats) ++pe_stats_[pe_id].total_translations;

        if (lookupTLBLocked(pe_id, virtual_page, physical_page)) {
            hit = true;
            ++whole_program_pe_stats_[pe_id].tlb_hits;
            if (collect_stats) ++pe_stats_[pe_id].tlb_hits;
            PageTableEntry* pte = findPTE(virtual_page);
            if (pte && pte->valid &&
                (!pte->referenced || (is_write && !pte->dirty))) {
                const std::vector<Address> pte_addresses =
                    getPteReadAddressesLocked(virtual_page);
                if (!pte_addresses.empty()) {
                    hit_pte_address = pte_addresses.back();
                    hit_needs_pte_update = true;
                }
            }
            markAccessedLocked(virtual_page, is_write);
        } else {
            hit = false;
            ++whole_program_pe_stats_[pe_id].tlb_misses;
            if (collect_stats) ++pe_stats_[pe_id].tlb_misses;
        }
    }

    if (hit) {
        // The first store after a read can hit in the TLB while the PTE's dirty
        // bit is still clear. Model that hardware A/D update as a PTE write.
        if (hit_needs_pte_update) {
            latency += page_table_access(hit_pte_address, true);
            recordPteAccess(pe_id, true);
        }
        recordTranslationCycles(pe_id, latency);
        return (physical_page << kPageBits) | page_offset;
    }

    // PTE reads are dependent: each request completes before the walker issues
    // the next level. The callback injects these as ordinary PE-MI requests.
    for (;;) {
        std::vector<Address> pte_reads;
        bool mapped_at_walk_start = false;
        Address pfn_at_walk_start = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ensurePE(pe_id);
            ++whole_program_pe_stats_[pe_id].page_walks;
            if (stats_enabled_.load()) ++pe_stats_[pe_id].page_walks;
            pte_reads = getPteReadAddressesLocked(virtual_page);
            const PageTableEntry* pte = findPTE(virtual_page);
            mapped_at_walk_start = pte && pte->valid;
            if (mapped_at_walk_start) pfn_at_walk_start = pte->physical_page;
        }

        for (Address pte_address : pte_reads) {
            latency += page_table_access(pte_address, false);
            recordPteAccess(pe_id, false);
        }

        std::vector<Address> pte_writes;
        bool mapped = false;
        bool retry_walk = false;
        {
            std::unique_lock<std::mutex> lock(state_mutex_);
            PageTableEntry* pte = findPTE(virtual_page);
            if (pte && pte->installing) {
                // Coalesce a concurrent first-touch fault for this VPN. The
                // state lock is released while waiting, so its PTE writes can
                // complete through the same PE-MI path.
                state_changed_.wait(lock, [pte]() { return !pte->installing; });
                retry_walk = true;
            }
            if (retry_walk) {
                // Our reads may have observed the old invalid PTE. Retry after
                // the competing install has become visible.
            } else {
                const bool mapped_now = pte && pte->valid;
                if (mapped_at_walk_start && mapped_now &&
                    pte->physical_page == pfn_at_walk_start) {
                    physical_page = pte->physical_page;
                    const bool updateAccessedDirty =
                        !pte->referenced || (is_write && !pte->dirty);
                    if (updateAccessedDirty) {
                        const std::vector<Address> pte_addresses =
                            getPteReadAddressesLocked(virtual_page);
                        if (!pte_addresses.empty()) pte_writes.push_back(pte_addresses.back());
                    }
                    markAccessedLocked(virtual_page, is_write);
                    updateTLB(pe_id, virtual_page, physical_page);
                    mapped = true;
                } else if (mapped_now) {
                    // Another PE may have faulted this VPN while our failed PTE
                    // reads were in flight, or the mapping may have been replaced.
                    // Retry so the walker actually observes the installed entry.
                    retry_walk = true;
                } else {
                    // A simulated minor fault allocates page-table/data frames and
                    // installs the missing PTEs. Its memory-visible cost is the
                    // resulting writes below; handler CPU time remains out of scope.
                    ++whole_program_pe_stats_[pe_id].page_faults;
                    if (stats_enabled_.load()) ++pe_stats_[pe_id].page_faults;
                    pte_writes = installPageLocked(virtual_page, pe_id);
                }
            }
        }

        if (mapped) {
            // Model the hardware A/D-bit update as a page-table write on the
            // walk that first sets either bit. TLB hits do not revisit the PTE.
            for (Address pte_address : pte_writes) {
                latency += page_table_access(pte_address, true);
                recordPteAccess(pe_id, true);
            }
            break;
        }
        if (retry_walk) continue;

        for (Address pte_address : pte_writes) {
            latency += page_table_access(pte_address, true);
            recordPteAccess(pe_id, true);
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            PageTableEntry* pte = findPTE(virtual_page);
            if (pte && pte->installing) {
                pte->valid = true;
                pte->installing = false;
                ++mapped_pages_;
            }
        }
        state_changed_.notify_all();
        // Hardware retries after the fault handler installs the PTEs.
    }

    recordTranslationCycles(pe_id, latency);
    return (physical_page << kPageBits) | page_offset;
}

void AddressTranslator::invalidateTLB(uint32_t pe_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pe_id >= pe_tlbs_.size()) return;
    for (auto& entry : pe_tlbs_[pe_id]) entry.valid = false;
}

void AddressTranslator::invalidateTLBPage(Address virtual_page) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    invalidateTLBPageLocked(virtual_page);
}

void AddressTranslator::invalidatePage(Address virtual_page) {
    unmapPage(virtual_page);
}

void AddressTranslator::mapPage(Address virtual_page, Address physical_page,
                                uint32_t pe_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ensurePE(pe_id);
    const auto indices = getPageTableIndices(virtual_page);
    auto& middle = page_table_.entries[indices[0]];
    if (!middle) middle = std::make_unique<MiddlePageTable>(allocatePageTableFrameLocked());
    auto& leaf = middle->entries[indices[1]];
    if (!leaf) leaf = std::make_unique<LeafPageTable>(allocatePageTableFrameLocked());

    PageTableEntry& pte = leaf->entries[indices[2]];
    if (!pte.valid) ++mapped_pages_;
    invalidateTLBPageLocked(virtual_page);
    pte.physical_page = physical_page;
    pte.valid = true;
    pte.installing = false;
    pte.dirty = false;
    pte.referenced = false;
    pte.owner_pe = pe_id;
    if (physical_page >= next_physical_page_) next_physical_page_ = physical_page + 1;
    state_changed_.notify_all();
}

Address AddressTranslator::allocatePage(Address virtual_page, uint32_t pe_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ensurePE(pe_id);
    return allocatePageLocked(virtual_page, pe_id);
}

void AddressTranslator::unmapPage(Address virtual_page) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    PageTableEntry* pte = findPTE(virtual_page);
    if (pte && pte->valid) {
        pte->valid = false;
        pte->installing = false;
        --mapped_pages_;
    }
    invalidateTLBPageLocked(virtual_page);
}

bool AddressTranslator::isPageMapped(Address virtual_page) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const PageTableEntry* pte = findPTE(virtual_page);
    return pte && pte->valid;
}

void AddressTranslator::insertTLB(uint32_t pe_id, Address virtual_page,
                                  Address physical_page) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    updateTLB(pe_id, virtual_page, physical_page);
}

bool AddressTranslator::lookupTLB(uint32_t pe_id, Address virtual_page,
                                  Address& physical_page) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return lookupTLBLocked(pe_id, virtual_page, physical_page);
}

void AddressTranslator::flushTLB(uint32_t pe_id) {
    invalidateTLB(pe_id);
}

AddressTranslator::TranslationStats
AddressTranslator::getStats(uint32_t pe_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pe_id < pe_stats_.size()) return pe_stats_[pe_id];
    return TranslationStats();
}

void AddressTranslator::printStats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto printGroup = [](const char* heading,
                         const std::vector<TranslationStats>& stats_by_pe) {
        std::cout << "\n=== Address Translation Statistics (" << heading << ") ==="
                  << std::endl;
        if (stats_by_pe.empty()) {
            std::cout << "No translation statistics available." << std::endl;
            return;
        }

        TranslationStats total;
        bool any = false;
        std::cout << "\nPer-PE Statistics:" << std::endl;
        for (size_t pe = 0; pe < stats_by_pe.size(); ++pe) {
            const TranslationStats& s = stats_by_pe[pe];
            if (s.total_translations == 0 && s.page_faults == 0) continue;
            any = true;
            total.total_translations += s.total_translations;
            total.tlb_hits += s.tlb_hits;
            total.tlb_misses += s.tlb_misses;
            total.page_walks += s.page_walks;
            total.page_table_reads += s.page_table_reads;
            total.page_table_writes += s.page_table_writes;
            total.page_faults += s.page_faults;
            total.translation_cycles += s.translation_cycles;

            const double hit_rate = s.total_translations
                ? (s.tlb_hits * 100.0) / s.total_translations : 0.0;
            std::cout << "  PE " << pe << ":\n"
                      << "    Total translations: " << s.total_translations << "\n"
                      << "    TLB hits: " << s.tlb_hits << " (" << hit_rate << "%)\n"
                      << "    TLB misses: " << s.tlb_misses << "\n"
                      << "    Page walks: " << s.page_walks << "\n"
                      << "    PTE reads: " << s.page_table_reads << "\n"
                      << "    PTE writes: " << s.page_table_writes << "\n"
                      << "    First-touch faults: " << s.page_faults << "\n"
                      << "    Translation cycles: " << s.translation_cycles << std::endl;
        }

        if (!any) {
            std::cout << "No translation statistics available." << std::endl;
            return;
        }
        std::cout << "\nAggregate Statistics:\n"
                  << "  Total translations: " << total.total_translations << "\n"
                  << "  Total TLB hits: " << total.tlb_hits << "\n"
                  << "  Total TLB misses: " << total.tlb_misses << "\n"
                  << "  Total page walks: " << total.page_walks << "\n"
                  << "  Total PTE reads: " << total.page_table_reads << "\n"
                  << "  Total PTE writes: " << total.page_table_writes << "\n"
                  << "  Total first-touch faults: " << total.page_faults << "\n"
                  << "  Total translation cycles: " << total.translation_cycles << std::endl;
        if (total.total_translations) {
            std::cout << "  Average translation latency: "
                      << static_cast<double>(total.translation_cycles) /
                             total.total_translations
                      << " cycles/translation\n"
                      << "  Overall TLB hit rate: "
                      << (total.tlb_hits * 100.0) / total.total_translations
                      << "%" << std::endl;
        }
    };

    printGroup("ROI", pe_stats_);
    printGroup("Whole Program", whole_program_pe_stats_);

    std::cout << "\nPage Table Statistics:\n"
              << "  Mapped 4 KiB pages: " << mapped_pages_ << "\n"
              << "  Levels: " << kLevels << "\n"
              << "  Entries per level: " << kEntriesPerLevel << std::endl;

    std::cout << "\nTLB Configuration:\n"
              << "  TLB entries: " << config_.tlb_entries << "\n"
              << "  TLB associativity: " << config_.tlb_associativity << "\n"
              << "  Page size: " << config_.page_size_bytes << " bytes\n"
              << "  TLB lookup latency: " << config_.tlb_lookup_latency << " cycles\n"
              << "  PTE service time: PE memory-interface latency" << std::endl;
}

void AddressTranslator::resetStats() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pe_stats_.clear();
}

Address AddressTranslator::getPageNumber(Address addr) const {
    constexpr Address virtual_mask = (Address{1} << kVirtualAddressBits) - 1;
    return (addr & virtual_mask) >> kPageBits;
}

Address AddressTranslator::getPageOffset(Address addr) const {
    return addr & ((Address{1} << kPageBits) - 1);
}

bool AddressTranslator::isCanonicalAddress(Address addr) const {
    const uint64_t upper = addr >> kVirtualAddressBits;
    const bool sign = ((addr >> (kVirtualAddressBits - 1)) & 1) != 0;
    return sign ? upper == 0xFFFF : upper == 0;
}

std::array<uint32_t, AddressTranslator::kLevels>
AddressTranslator::getPageTableIndices(Address virtual_page) const {
    constexpr Address vpn_mask =
        (Address{1} << (kVirtualAddressBits - kPageBits)) - 1;
    virtual_page &= vpn_mask;
    return {{
        static_cast<uint32_t>((virtual_page >> (2 * kIndexBits)) & (kEntriesPerLevel - 1)),
        static_cast<uint32_t>((virtual_page >> kIndexBits) & (kEntriesPerLevel - 1)),
        static_cast<uint32_t>(virtual_page & (kEntriesPerLevel - 1))
    }};
}

PageTableEntry* AddressTranslator::findPTE(Address virtual_page) {
    const auto indices = getPageTableIndices(virtual_page);
    const auto& middle = page_table_.entries[indices[0]];
    if (!middle) return nullptr;
    const auto& leaf = middle->entries[indices[1]];
    if (!leaf) return nullptr;
    return &leaf->entries[indices[2]];
}

const PageTableEntry* AddressTranslator::findPTE(Address virtual_page) const {
    const auto indices = getPageTableIndices(virtual_page);
    const auto& middle = page_table_.entries[indices[0]];
    if (!middle) return nullptr;
    const auto& leaf = middle->entries[indices[1]];
    if (!leaf) return nullptr;
    return &leaf->entries[indices[2]];
}

std::vector<Address>
AddressTranslator::getPteReadAddressesLocked(Address virtual_page) const {
    const auto indices = getPageTableIndices(virtual_page);
    std::vector<Address> addresses;
    addresses.reserve(kLevels);
    addresses.push_back((page_table_.physical_page << kPageBits) +
                        static_cast<Address>(indices[0]) * kPteBytes);
    const auto& middle = page_table_.entries[indices[0]];
    if (!middle) return addresses;

    addresses.push_back((middle->physical_page << kPageBits) +
                        static_cast<Address>(indices[1]) * kPteBytes);
    const auto& leaf = middle->entries[indices[1]];
    if (!leaf) return addresses;

    addresses.push_back((leaf->physical_page << kPageBits) +
                        static_cast<Address>(indices[2]) * kPteBytes);
    return addresses;
}

std::vector<Address>
AddressTranslator::installPageLocked(Address virtual_page, uint32_t pe_id) {
    const auto indices = getPageTableIndices(virtual_page);
    std::vector<Address> writes;
    writes.reserve(kLevels);

    auto& middle = page_table_.entries[indices[0]];
    if (!middle) {
        middle = std::make_unique<MiddlePageTable>(allocatePageTableFrameLocked());
        writes.push_back((page_table_.physical_page << kPageBits) +
                         static_cast<Address>(indices[0]) * kPteBytes);
    }

    auto& leaf = middle->entries[indices[1]];
    if (!leaf) {
        leaf = std::make_unique<LeafPageTable>(allocatePageTableFrameLocked());
        writes.push_back((middle->physical_page << kPageBits) +
                         static_cast<Address>(indices[1]) * kPteBytes);
    }

    PageTableEntry& pte = leaf->entries[indices[2]];
    if (!pte.valid && !pte.installing) {
        pte.physical_page = next_physical_page_++;
        pte.installing = true;
        pte.dirty = false;
        pte.referenced = false;
        pte.owner_pe = pe_id;
        writes.push_back((leaf->physical_page << kPageBits) +
                         static_cast<Address>(indices[2]) * kPteBytes);
    }
    invalidateTLBPageLocked(virtual_page);
    return writes;
}

Address AddressTranslator::allocatePageLocked(Address virtual_page, uint32_t pe_id) {
    if (const PageTableEntry* pte = findPTE(virtual_page);
        pte && (pte->valid || pte->installing)) {
        return pte->physical_page;
    }
    (void)installPageLocked(virtual_page, pe_id);
    return findPTE(virtual_page)->physical_page;
}

Address AddressTranslator::allocatePageTableFrameLocked() {
    const Address remainder = next_physical_page_ % kPageTablePages;
    if (remainder) next_physical_page_ += kPageTablePages - remainder;
    const Address base = next_physical_page_;
    next_physical_page_ += kPageTablePages;
    return base;
}

void AddressTranslator::markAccessedLocked(Address virtual_page, bool is_write) {
    PageTableEntry* pte = findPTE(virtual_page);
    if (!pte || !pte->valid) return;
    pte->referenced = true;
    if (is_write) pte->dirty = true;
}

void AddressTranslator::invalidateTLBPageLocked(Address virtual_page) {
    const Address mask = (Address{1} << (kVirtualAddressBits - kPageBits)) - 1;
    virtual_page &= mask;
    for (auto& tlb : pe_tlbs_) {
        for (auto& entry : tlb) {
            if (entry.valid && entry.virtual_page == virtual_page) entry.valid = false;
        }
    }
}

void AddressTranslator::ensurePE(uint32_t pe_id) {
    const size_t needed = static_cast<size_t>(pe_id) + 1;
    if (pe_tlbs_.size() < needed) pe_tlbs_.resize(needed);
    if (pe_stats_.size() < needed) pe_stats_.resize(needed);
    if (whole_program_pe_stats_.size() < needed) {
        whole_program_pe_stats_.resize(needed);
    }
    if (pe_tlbs_[pe_id].empty()) pe_tlbs_[pe_id].resize(config_.tlb_entries);
}

bool AddressTranslator::lookupTLBLocked(uint32_t pe_id, Address virtual_page,
                                        Address& physical_page) {
    ensurePE(pe_id);
    auto& tlb = pe_tlbs_[pe_id];
    const uint32_t entries_per_set = config_.tlb_associativity;
    const uint32_t set_start = getTLBSet(virtual_page) * entries_per_set;

    for (uint32_t way = 0; way < entries_per_set; ++way) {
        const uint32_t idx = set_start + way;
        if (tlb[idx].valid && tlb[idx].virtual_page == virtual_page) {
            physical_page = tlb[idx].physical_page;
            tlb[idx].last_access = ++tlb_clock_;
            return true;
        }
    }
    return false;
}

void AddressTranslator::updateTLB(uint32_t pe_id, Address virtual_page,
                                  Address physical_page) {
    ensurePE(pe_id);
    auto& tlb = pe_tlbs_[pe_id];
    const uint32_t entries_per_set = config_.tlb_associativity;
    const uint32_t set_start = getTLBSet(virtual_page) * entries_per_set;

    for (uint32_t way = 0; way < entries_per_set; ++way) {
        const uint32_t idx = set_start + way;
        if (!tlb[idx].valid) {
            tlb[idx].virtual_page = virtual_page;
            tlb[idx].physical_page = physical_page;
            tlb[idx].valid = true;
            tlb[idx].last_access = ++tlb_clock_;
            return;
        }
    }

    uint32_t lru_idx = set_start;
    for (uint32_t way = 1; way < entries_per_set; ++way) {
        const uint32_t idx = set_start + way;
        if (tlb[idx].last_access < tlb[lru_idx].last_access) lru_idx = idx;
    }
    tlb[lru_idx].virtual_page = virtual_page;
    tlb[lru_idx].physical_page = physical_page;
    tlb[lru_idx].valid = true;
    tlb[lru_idx].last_access = ++tlb_clock_;
}

void AddressTranslator::recordPteAccess(uint32_t pe_id, bool is_write) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ensurePE(pe_id);
    if (is_write) {
        ++whole_program_pe_stats_[pe_id].page_table_writes;
        if (stats_enabled_.load()) ++pe_stats_[pe_id].page_table_writes;
    } else {
        ++whole_program_pe_stats_[pe_id].page_table_reads;
        if (stats_enabled_.load()) ++pe_stats_[pe_id].page_table_reads;
    }
}

void AddressTranslator::recordTranslationCycles(uint32_t pe_id, Cycle cycles) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ensurePE(pe_id);
    whole_program_pe_stats_[pe_id].translation_cycles += cycles;
    if (stats_enabled_.load()) pe_stats_[pe_id].translation_cycles += cycles;
}

uint32_t AddressTranslator::getTLBSet(Address virtual_page) const {
    const uint32_t num_sets = config_.tlb_entries / config_.tlb_associativity;
    return num_sets ? static_cast<uint32_t>(virtual_page % num_sets) : 0;
}

} // namespace pimid
