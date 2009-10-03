/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

/**
 * raidxor_fill_page() - fills page with a value
 *
 * Copies value length times into the page buffer.
 */
static void raidxor_fill_page(struct page *page, unsigned char value,
			      unsigned long length)
{
	unsigned char *data = kmap(page);
	memset(data, value, length);
	kunmap(page);
}

static int raidxor_test_case_xor_combine_encode(void)
{
	unsigned long i;
	struct bio bio1, bio2, bio3;
	struct bio_vec vs1[2], vs2[2], vs3[2];
	unsigned char xor1, xor2;
	unsigned char *data;
	raidxor_bio_t *rxbio;
	encoding_t *encoding;
	mdk_rdev_t rdev1, rdev2, rdev3;
	struct disk_info unit1, unit2, unit3;
	int result = 0;

	printk(KERN_EMERG "bio1 is %p\n", &bio1); 
	printk(KERN_EMERG "bio2 is %p\n", &bio2);
	printk(KERN_EMERG "bio3 is %p\n", &bio3);

	unit1.rdev = &rdev1;
	unit2.rdev = &rdev2;
	unit3.rdev = &rdev3;
	unit1.redundant = unit2.redundant = 0;
	unit3.redundant = 1;
	unit1.encoding = unit2.encoding = NULL;
	unit1.resource = unit2.resource = NULL;

	bio1.bi_bdev = rdev1.bdev = (void *) 0xdeadbeef;
	bio2.bi_bdev = rdev2.bdev = (void *) 0xcafecafe;
	bio3.bi_bdev = rdev3.bdev = (void *) 0xf000f000;

	rxbio = kzalloc(sizeof(raidxor_bio_t) +
			sizeof(struct bio *) * 3, GFP_NOIO);
	if (!rxbio) {
		printk(KERN_EMERG "raidxor: allocation failed in test case"
		       " xor_combine_encode\n");
		return 1;
	}

	rxbio->n_bios = 3;
	rxbio->bios[0] = &bio1;
	rxbio->bios[1] = &bio2;
	rxbio->bios[2] = &bio3;

	encoding = kzalloc(sizeof(encoding_t) +
			   sizeof(disk_info_t *) * 2, GFP_NOIO);
	if (!encoding) {
		kfree(rxbio);
		printk(KERN_EMERG "raidxor: allocation failed in test case"
		       " xor_combine_encode\n");
		return 1;
	}

	encoding->n_units = 2;
	encoding->units[0] = &unit1;
	encoding->units[1] = &unit2;
       
	unit3.encoding = encoding;

	bio1.bi_io_vec = vs1;
	bio2.bi_io_vec = vs2;
	bio3.bi_io_vec = vs3;

	bio3.bi_vcnt = bio2.bi_vcnt = bio1.bi_vcnt = 2;

	vs1[0].bv_len = vs1[1].bv_len = PAGE_SIZE;
	vs2[0].bv_len = vs2[1].bv_len = PAGE_SIZE;
	vs3[0].bv_len = vs3[1].bv_len = PAGE_SIZE;

	vs1[0].bv_offset = vs1[1].bv_offset = vs1[2].bv_offset = 0;
	vs2[0].bv_offset = vs2[1].bv_offset = vs2[2].bv_offset = 0;
	vs3[0].bv_offset = vs3[1].bv_offset = vs3[2].bv_offset = 0;

	bio3.bi_size = bio2.bi_size = bio1.bi_size = 2 * PAGE_SIZE;

	vs1[0].bv_page = alloc_page(GFP_NOIO);
	vs1[1].bv_page = alloc_page(GFP_NOIO);
	vs2[0].bv_page = alloc_page(GFP_NOIO);
	vs2[1].bv_page = alloc_page(GFP_NOIO);
	vs3[0].bv_page = alloc_page(GFP_NOIO);
	vs3[1].bv_page = alloc_page(GFP_NOIO);

	raidxor_fill_page(vs1[0].bv_page, 3, PAGE_SIZE);
	raidxor_fill_page(vs2[0].bv_page, 180, PAGE_SIZE);

	data = __bio_kmap_atomic(&bio1, 0, KM_USER0);
	if (data[0] != 3)
		printk(KERN_EMERG "bio1[0][0] == %u != %u\n", data[0], 3);
	__bio_kunmap_atomic(data, KM_USER0);

	data = __bio_kmap_atomic(&bio2, 0, KM_USER0);
	if (data[0] != 180)
		printk(KERN_EMERG "bio2[0][0] == %u != %u\n", data[0], 180);
	__bio_kunmap_atomic(data, KM_USER0);

	xor1 = 3 ^ 180;

	raidxor_fill_page(vs1[1].bv_page, 15, PAGE_SIZE);
	raidxor_fill_page(vs2[1].bv_page, 23, PAGE_SIZE);

	xor2 = 15 ^ 23;

	raidxor_xor_combine_encode(&bio3, rxbio, encoding);

	data = __bio_kmap_atomic(&bio3, 0, KM_USER0);
	printk(KERN_EMERG "data lies at %p\n", data);
	for (i = 0; i < PAGE_SIZE; ++i) {
		if (data[i] != xor1) {
			printk(KERN_EMERG "raidxor: buffer 1 differs at byte"
			       " %lu: %d != %d\n", i, data[i], xor1);
			result = 1;
			goto out;
		}
	}
	__bio_kunmap_atomic(data, KM_USER0);

	data = __bio_kmap_atomic(&bio3, 1, KM_USER0);
	for (i = 0; i < PAGE_SIZE; ++i) {
		if (data[i] != xor2) {
			printk(KERN_EMERG "raidxor: buffer 2 differs at byte"
			       " %lu: %d != %d\n", i, data[i], xor2);
			result = 1;
			goto out;
		}
	}
out:
	__bio_kunmap_atomic(data, KM_USER0);

	safe_put_page(vs1[0].bv_page);
	safe_put_page(vs1[1].bv_page);
	safe_put_page(vs2[0].bv_page);
	safe_put_page(vs2[1].bv_page);
	safe_put_page(vs3[0].bv_page);
	safe_put_page(vs3[1].bv_page);

	kfree(rxbio);
	kfree(encoding);

	return result;
}

static int raidxor_test_case_sizeandlayout(void)
{
	int result;
	struct bio bio1, bio2;
	struct bio_vec vs1[2], vs2[2];

	bio1.bi_io_vec = vs1;
	bio2.bi_io_vec = vs2;

	bio2.bi_vcnt = bio1.bi_vcnt = 2;

	vs2[0].bv_len = vs1[0].bv_len = 42;
	vs2[1].bv_len = vs1[1].bv_len = 1024;

	bio2.bi_size = bio1.bi_size = 42 + 1024;

	if ((result = raidxor_check_same_size_and_layout(&bio1, &bio2))) {
		printk(KERN_EMERG "raidxor: test case sizeandlayout/1 failed: %d\n",
		       result);
		return 1;
	}

	vs1[1].bv_len = 32;

	if (!(result = raidxor_check_same_size_and_layout(&bio1, &bio2))) {
		printk(KERN_EMERG "raidxor: test case sizeandlayout/2 failed: %d\n",
		       result);
		return 1;
	}

	return 0;
}

static int raidxor_test_case_find_bio(void)
{
	struct bio bio1, bio2, bio3;
	raidxor_bio_t *rxbio;
	mdk_rdev_t rdev1, rdev2;
	struct disk_info unit1, unit2;

	unit1.rdev = &rdev1;
	unit2.rdev = &rdev2;
	unit1.redundant = unit2.redundant = 0;
	unit1.encoding = unit2.encoding = NULL;
	unit1.resource = unit2.resource = NULL;

	bio1.bi_bdev = rdev1.bdev = (void *) 0xdeadbeef;
	bio2.bi_bdev = rdev2.bdev = (void *) 0xcafecafe;

	rxbio = kzalloc(sizeof(raidxor_bio_t) +
			sizeof(struct bio *) * 3, GFP_NOIO);
	if (!rxbio) {
		printk(KERN_EMERG "raidxor: allocation failed in test case"
		       " xor_combine_encode\n");
		return 1;
	}

	rxbio->n_bios = 3;
	rxbio->bios[0] = &bio1;
	rxbio->bios[1] = &bio2;
	rxbio->bios[2] = &bio3;

	if (&bio1 != raidxor_find_bio(rxbio, &unit1)) {
		kfree(rxbio);
		printk(KERN_EMERG "raidxor: didn't find unit 1 in test case"
		       " find_bio\n");
		return 1;
	}

	if (&bio2 != raidxor_find_bio(rxbio, &unit2)) {
		kfree(rxbio);
		printk(KERN_EMERG "raidxor: didn't find unit 2 in test case"
		       " find_bio\n");
		return 1;
	}

	kfree(rxbio);

	return 0;
}

static int raidxor_test_case_xor_single(void)
{
	unsigned long i;
	struct bio bio1, bio2;
	struct bio_vec vs1[2], vs2[2];
	unsigned char xor1, xor2;
	unsigned char *data;
	int result = 0;

	bio1.bi_io_vec = vs1;
	bio2.bi_io_vec = vs2;

	bio2.bi_vcnt = bio1.bi_vcnt = 2;

	vs2[0].bv_len = vs1[0].bv_len = PAGE_SIZE;
	vs2[1].bv_len = vs1[1].bv_len = PAGE_SIZE;
	vs1[0].bv_offset = vs1[1].bv_offset = 0;
	vs2[0].bv_offset = vs2[1].bv_offset = 0;

	bio2.bi_size = bio1.bi_size = 4096 * 2;

	vs1[0].bv_page = alloc_page(GFP_NOIO);
	vs1[1].bv_page = alloc_page(GFP_NOIO);
	vs2[0].bv_page = alloc_page(GFP_NOIO);
	vs2[1].bv_page = alloc_page(GFP_NOIO);

	printk(KERN_EMERG "*** first!\n");

	raidxor_fill_page(vs1[0].bv_page, 3, PAGE_SIZE);
	raidxor_fill_page(vs1[1].bv_page, 42, PAGE_SIZE);

	printk(KERN_EMERG "*** second!\n");

	raidxor_fill_page(vs2[0].bv_page, 15, PAGE_SIZE);
	raidxor_fill_page(vs2[1].bv_page, 23, PAGE_SIZE);

	xor1 = 3 ^ 15;
	xor2 = 42 ^ 23;

	printk(KERN_EMERG "*** third!\n");

	raidxor_xor_single(&bio1, &bio2);

	printk(KERN_EMERG "*** fourth!\n");

	/* raidxor: buffer 1 differs at byte 0: 3 != 41 */
	data = kmap(vs1[0].bv_page);
	for (i = 0; i < PAGE_SIZE; ++i) {
		if (data[i] != xor1) {
			printk(KERN_EMERG "raidxor: buffer 1 differs at byte"
			       " %lu: %d != %d\n", i, data[i], xor1);
			kunmap(vs1[0].bv_page);
			result = 1;
			goto out;
		}
	}
	kunmap(vs1[0].bv_page);

	printk(KERN_EMERG "*** fifth!\n");

	data = kmap(vs1[1].bv_page);
	for (i = 0; i < PAGE_SIZE; ++i) {
		if (data[i] != xor2) {
			printk(KERN_EMERG "raidxor: buffer 2 differs at byte"
			       " %lu: %d != %d\n", i, data[i], xor2);
			kunmap(vs1[1].bv_page);
			result = 1;
			goto out;
		}
	}
	kunmap(vs1[1].bv_page);

	printk(KERN_EMERG "*** sixth!\n");

out:
	safe_put_page(vs1[0].bv_page);
	safe_put_page(vs1[1].bv_page);
	safe_put_page(vs2[0].bv_page);
	safe_put_page(vs2[1].bv_page);

	return result;
}

static int raidxor_run_test_cases(void)
{
	printk(KERN_EMERG "raidxor: running test case sizeandlayout\n");
	if (raidxor_test_case_sizeandlayout()) {
		printk(KERN_EMERG "raidxor: test case sizeandlayout failed");
		return 1;
	}

	printk(KERN_EMERG "raidxor: running test case xor_single\n");
	if (raidxor_test_case_xor_single()) {
		printk(KERN_EMERG "raidxor: test case xor_single failed");
		return 1;
	}

	printk(KERN_EMERG "raidxor: running test case find_bio\n");
	if (raidxor_test_case_find_bio()) {
		printk(KERN_EMERG "raidxor: test case find_bio failed");
		return 1;
	}

	printk(KERN_EMERG "raidxor: running test case xor_combine_encode\n");
	if (raidxor_test_case_xor_combine_encode()) {
		printk(KERN_EMERG "raidxor: test case xor_combine_encode failed");
		return 1;
	}

	printk(KERN_EMERG "raidxor: test cases run successfully\n");
	return 0;
}

#if 0
Local variables:
c-basic-offset: 8
End:
#endif
