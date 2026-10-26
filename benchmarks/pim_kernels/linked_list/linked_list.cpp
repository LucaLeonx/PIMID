#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>

#include "zsim_hooks.h"

namespace {

constexpr std::size_t kDefaultObjects = 16384;
constexpr std::size_t kDefaultPasses = 10;

struct ListObject {
    std::uint64_t value;
    std::uint64_t payload[7];
};

bool parse_size_arg(int argc, char** argv, const char* flag,
                    std::size_t default_value, std::size_t* result) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) != 0) {
            continue;
        }

        char* end = nullptr;
        errno = 0;
        const unsigned long long value = std::strtoull(argv[i + 1], &end, 10);
        if (errno != 0 || end == argv[i + 1] || *end != '\0' || value == 0) {
            return false;
        }
        *result = static_cast<std::size_t>(value);
        return true;
    }

    *result = default_value;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t object_count = 0;
    std::size_t passes = 0;
    if (!parse_size_arg(argc, argv, "--objects", kDefaultObjects, &object_count) ||
        !parse_size_arg(argc, argv, "--passes", kDefaultPasses, &passes)) {
        std::fprintf(stderr,
                     "usage: %s [--objects positive_integer] "
                     "[--passes positive_integer]\n",
                     argv[0]);
        return 1;
    }

    std::list<ListObject> objects;
    for (std::size_t i = 0; i < object_count; ++i) {
        ListObject object{};
        object.value = static_cast<std::uint64_t>(i);
        for (std::size_t j = 0; j < 7; ++j) {
            object.payload[j] = static_cast<std::uint64_t>(i + j + 1);
        }
        objects.push_back(object);
    }

    std::uint64_t checksum = 0;
    zsim_roi_begin();
    for (std::size_t pass = 0; pass < passes; ++pass) {
        for (ListObject& object : objects) {
            checksum += object.value;
            checksum += object.payload[pass % 7];
            object.value += static_cast<std::uint64_t>(pass + 1);
        }
    }
    zsim_roi_end();

    std::printf("BENCH_CHECKSUM: %llu\n",
                static_cast<unsigned long long>(checksum));
    std::printf("BENCH_DONE\n");
    return 0;
}