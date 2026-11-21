/*
 * Application arguments, carried in a reserved block on the boot disk.
 *
 * Embed the arguments in the boot disk itself. The GPT image leaves LBA 34..2047 
 * unused between the primary partition table and the ESP (scripts/mkuefi.sh), 
 * so one sector in that gap carries a magic-tagged, NUL-terminated argument string.
 * Nothing but this code interprets it, so it works the same under QEMU and on 
 * any cloud, on both architectures.
 *
 * This is the same trick OSv's scripts/imgedit.py used on the old MBR images,
 * moved to a GPT-safe offset. scripts/setargs.py rewrites the block in place,
 * so changing arguments does not mean rebuilding the kernel.
 */

#ifndef OSV_BOOTARGS_HH
#define OSV_BOOTARGS_HH

#include <string>
#include <vector>

namespace osv {

// Sector holding the block, and its on-disk layout. LBA 34 is the first sector
// after the GPT partition entry array on a 512-byte-sector disk.
static const unsigned bootargs_lba = 34;
static const char bootargs_magic[8] = {'M', 'I', 'N', 'I', 'A', 'R', 'G', 'S'};
static const unsigned bootargs_max = 448;

struct bootargs_block {
    char magic[8];              // "MINIARGS"
    uint32_t length;            // bytes of args, excluding the NUL
    uint32_t reserved;
    char args[bootargs_max];    // NUL-terminated
};

// Read the block and return the argument string, or an empty string if the block is not present or invalid.
std::string bootargs();

// Split into whitespace-separated words, honouring single and double quotes.
std::vector<std::string> bootargs_split(const std::string &line);

} // namespace osv

#endif /* OSV_BOOTARGS_HH */
