/* vita-elf-create appends module metadata to the executable segment. */
SECTIONS { . = . + 0x1000; }
INSERT BEFORE .init_array;
