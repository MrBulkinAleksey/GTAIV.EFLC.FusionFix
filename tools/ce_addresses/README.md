# CE addresses from IV-SDK .NET

`ivsdk_ce_addresses.tsv` resolves the Complete Edition (1.2.0.59) entries of IV-SDK .NET's
`12059.ini` and `Patterns.ini` (Kev7cks/IV-SDK-DotNet, `IVSDKDotNet/Memory Offset Config Files/IVSDK/`)
against our GTAIV.exe, and marks which ones FusionFix already reaches.

Columns: `addr_from_12059` is the global or function the ini entry points at (its values are RVAs
near an operand, not always the operand itself, so the covering instruction's global is taken);
`from_pattern` is what the CE pattern finds (`func?` rows can be a site inside a function, not its
start); `matches` is the pattern's hit count; `we_have` says how our own patterns reach it
(`site` near one of our pattern hits, `global`/`call` referenced there, `literal` named in source).
Where the two addresses differ, check the code before using either.

Regenerate (Python with capstone and pefile, e.g. a uv venv):

    python build_image.py bin/GTAIV.exe log/<full dump>.dmp image.bin   # decrypted, un-relocated image
    python compare.py image.bin 12059.ini Patterns.ini source ivsdk_ce_addresses.tsv

`build_image.py` needs a full (-ma) dump of the game: the start of .text is encrypted in the file.
The image it writes is indexed by RVA (VA − 0x400000).
