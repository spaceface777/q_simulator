/* Collect the DWARF unwind tables under the 8-character PE section name that
 * LLVM's libunwind looks for. GNU ld would store the full ".eh_frame" name in
 * the string table once the image has long section names, such as debug ones. */
SECTIONS
{
  .eh_fram BLOCK(__section_alignment__) : { KEEP(*(.eh_frame*)) }
} INSERT AFTER .data;
