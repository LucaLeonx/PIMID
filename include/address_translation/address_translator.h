#ifndef PIMID_ADDRESS_TRANSLATOR_H
#define PIMID_ADDRESS_TRANSLATOR_H

#include "common/types.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace pimid {

/** A leaf PTE for the initial three-level, 48-bit PIM page table. */
struct PageTableEntry {
    Address physical_page;  // Physical page frame number (PFN), not byte address.
    bool valid;
    bool installing;
    bool dirty;
    bool referenced;
    uint32_t owner_pe;

    PageTableEntry() : physical_page(0), valid(false), installing(false), dirty(false),
                       referenced(false), owner_pe(0) {}
};

/** A translation cached by one PIM PE's data TLB. */
struct TLBEntry {
    Address virtual_page;
    Address physical_page;
    bool valid;
    Cycle last_access;

    TLBEntry() : virtual_page(0), physical_page(0), valid(false), last_access(0) {}
};

/** Configuration for the first PIM MMU model: 4 KiB pages and a 3-level radix
 * table for 48-bit virtual addresses. PTE service time comes from the PE memory
 * interface; only the small TLB lookup is charged directly by the translator. */
struct AddressTranslationConfig {
    uint32_t page_size_bytes;
    uint32_t page_bits;
    uint32_t tlb_entries;
    uint32_t tlb_associativity;
    Cycle tlb_lookup_latency;

    AddressTranslationConfig() : page_size_bytes(4096), page_bits(12),
                                 tlb_entries(64), tlb_associativity(4),
                                 tlb_lookup_latency(1) {}
};

/** PIM-side address translator with per-PE TLBs and a shared three-level radix
 * page table. Page-table frames and data frames share one simulated PFN space.
 * On a TLB miss, translate() sends each dependent PTE address to the caller's
 * memory callback, which must model the request and return its service cycles. */
class AddressTranslator {
public:
    using PageTableAccess = std::function<Cycle(Address byte_address, bool is_write)>;

    AddressTranslator(const AddressTranslationConfig& config);

    Address translate(Address virtual_addr, uint32_t pe_id, bool& hit,
                      Cycle& latency, bool is_write,
                      const PageTableAccess& page_table_access);
    void invalidateTLB(uint32_t pe_id);
    void invalidateTLBPage(Address virtual_page);
    void invalidatePage(Address virtual_page);

    // Page-table management. virtual_page and physical_page are page numbers.
    void mapPage(Address virtual_page, Address physical_page, uint32_t pe_id);
    Address allocatePage(Address virtual_page, uint32_t pe_id);
    void unmapPage(Address virtual_page);
    bool isPageMapped(Address virtual_page) const;

    // TLB management. virtual_page and physical_page are page numbers.
    void insertTLB(uint32_t pe_id, Address virtual_page, Address physical_page);
    bool lookupTLB(uint32_t pe_id, Address virtual_page, Address& physical_page);
    void flushTLB(uint32_t pe_id);

    struct TranslationStats {
        uint64_t total_translations;
        uint64_t tlb_hits;
        uint64_t tlb_misses;
        uint64_t page_walks;
        uint64_t page_table_reads;
        uint64_t page_table_writes;
        uint64_t page_faults;
        uint64_t translation_cycles;

        TranslationStats() : total_translations(0), tlb_hits(0), tlb_misses(0),
                             page_walks(0), page_table_reads(0), page_table_writes(0),
                             page_faults(0), translation_cycles(0) {}
    };

    TranslationStats getStats(uint32_t pe_id) const;
    void printStats() const;
    void resetStats();
    void setStatsEnabled(bool enabled) { stats_enabled_.store(enabled); }

private:
    static constexpr uint32_t kPageBits = 12;
    static constexpr uint32_t kIndexBits = 12;
    static constexpr uint32_t kEntriesPerLevel = 1u << kIndexBits;
    static constexpr uint32_t kLevels = 3;
    static constexpr uint32_t kVirtualAddressBits = 48;
    static constexpr uint32_t kPteBytes = sizeof(uint64_t);
    static constexpr uint32_t kPageTableBytes = kEntriesPerLevel * kPteBytes;
    static constexpr uint32_t kPageTablePages =
        (kPageTableBytes + (1u << kPageBits) - 1) / (1u << kPageBits);

    struct LeafPageTable {
        Address physical_page;
        std::array<PageTableEntry, kEntriesPerLevel> entries{};
        explicit LeafPageTable(Address pfn) : physical_page(pfn) {}
    };

    struct MiddlePageTable {
        Address physical_page;
        std::array<std::unique_ptr<LeafPageTable>, kEntriesPerLevel> entries{};
        explicit MiddlePageTable(Address pfn) : physical_page(pfn) {}
    };

    struct RootPageTable {
        Address physical_page;
        std::array<std::unique_ptr<MiddlePageTable>, kEntriesPerLevel> entries{};
        explicit RootPageTable(Address pfn) : physical_page(pfn) {}
    };

    AddressTranslationConfig config_;
    RootPageTable page_table_;
    Address next_physical_page_ = 1;
    size_t mapped_pages_ = 0;
    Cycle tlb_clock_ = 0;
    std::atomic<bool> stats_enabled_{true};

    // The state lock protects table metadata, TLBs, counters, and PFN allocation.
    // It is released before each PE-MI request so independent walks can overlap.
    mutable std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::vector<std::vector<TLBEntry>> pe_tlbs_;
    std::vector<TranslationStats> pe_stats_;
    std::vector<TranslationStats> whole_program_pe_stats_;

    Address getPageNumber(Address addr) const;
    Address getPageOffset(Address addr) const;
    bool isCanonicalAddress(Address addr) const;
    std::array<uint32_t, kLevels> getPageTableIndices(Address virtual_page) const;
    PageTableEntry* findPTE(Address virtual_page);
    const PageTableEntry* findPTE(Address virtual_page) const;
    std::vector<Address> getPteReadAddressesLocked(Address virtual_page) const;
    std::vector<Address> installPageLocked(Address virtual_page, uint32_t pe_id);
    Address allocatePageTableFrameLocked();
    Address allocatePageLocked(Address virtual_page, uint32_t pe_id);
    void markAccessedLocked(Address virtual_page, bool is_write);
    void invalidateTLBPageLocked(Address virtual_page);
    void ensurePE(uint32_t pe_id);
    bool lookupTLBLocked(uint32_t pe_id, Address virtual_page, Address& physical_page);
    void updateTLB(uint32_t pe_id, Address virtual_page, Address physical_page);
    void recordPteAccess(uint32_t pe_id, bool is_write);
    void recordTranslationCycles(uint32_t pe_id, Cycle cycles);
    uint32_t getTLBSet(Address virtual_page) const;
};

} // namespace pimid

#endif // PIMID_ADDRESS_TRANSLATOR_H
