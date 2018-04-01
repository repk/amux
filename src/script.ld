SECTIONS {
	.rodata.poller : {
		__poller_start = .;
		KEEP(*(.rodata.poller))
		__poller_end = .;
	}
}

INSERT BEFORE .rodata;
