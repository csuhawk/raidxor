/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

/* for do_div on 64bit machines */
#include <asm/div64.h>

/* for xor_blocks */
#include <linux/raid/xor.h>

#include "raidxor.h"

#include "params.c"
#include "utils.c"
#include "conf.c"

static int raidxor_cache_make_clean(cache_t *cache, unsigned int line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	unsigned long flags;
	raidxor_conf_t *conf;

 	CHECK_FUN(raidxor_cache_make_clean);

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(line < cache->n_lines);

	conf = cache->conf;
	CHECK_PLAIN_RET_VAL(conf);

	WITHLOCKCONF(conf, flags, {
	if (cache->lines[line]->status == CACHE_LINE_CLEAN) {
		UNLOCKCONF(conf, flags);
		return 0;
	}

	if (cache->lines[line]->status != CACHE_LINE_READY ||
	    cache->lines[line]->status != CACHE_LINE_READYING) {
		UNLOCKCONF(conf, flags);
		return 1;
	}

	cache->lines[line]->status = CACHE_LINE_CLEAN;
	raidxor_cache_drop_line(cache, line);
	});

	return 0;
}

static int raidxor_cache_make_ready(cache_t *cache, unsigned int n_line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	unsigned int i;
	cache_line_t *line;
	unsigned long flags;
	raidxor_conf_t *conf;

 	CHECK_FUN(raidxor_cache_make_ready);

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(n_line < cache->n_lines);

	conf = cache->conf;
	CHECK_PLAIN_RET_VAL(conf);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET_VAL(line);

	WITHLOCKCONF(conf, flags, {

	if (line->status == CACHE_LINE_READY || line->status == CACHE_LINE_UPTODATE) {
		line->status = CACHE_LINE_READY;
		UNLOCKCONF(conf, flags);
		return 0;
	}

	if (line->status != CACHE_LINE_CLEAN) {
		UNLOCKCONF(conf, flags);
		return 1;
	}

	line->status = CACHE_LINE_READYING;
	});

	if (raidxor_cache_line_ensure_temps(cache, n_line))
		goto out_free_pages;

	for (i = 0; i < (cache->n_buffers + cache->n_red_buffers) * cache->n_chunk_mult; ++i) {
		if (!(line->buffers[i] = alloc_page(GFP_NOIO)))
			goto out_free_pages;
	}

	WITHLOCKCONF(conf, flags, {
	line->status = CACHE_LINE_READY;
	});

	return 0;
out_free_pages:
	raidxor_cache_make_clean(cache, n_line);
	return 1;
}

static int raidxor_cache_make_load_me(cache_t *cache, unsigned int line,
				      sector_t sector)
{
	CHECK_FUN(raidxor_cache_make_load_me);

#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(line < cache->n_lines);

	if (cache->lines[line]->status == CACHE_LINE_LOAD_ME) return 0;
	CHECK_PLAIN_RET_VAL(cache->lines[line]->status == CACHE_LINE_READY);

	cache->lines[line]->status = CACHE_LINE_LOAD_ME;
	cache->lines[line]->sector = sector;

	return 0;
}

/**
 * raidxor_valid_decoding() - checks for necessary equation(s)
 *
 * Returns 1 if everything is okay, else 0.
 */
static unsigned int raidxor_valid_decoding(cache_t *cache, unsigned int n_line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	cache_line_t *line;
	unsigned int i;
	raidxor_bio_t *rxbio;
	raidxor_conf_t *conf;

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(n_line < cache->n_lines);

	conf = cache->conf;
	CHECK_PLAIN_RET_VAL(conf);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET_VAL(line);

	rxbio = line->rxbio;
	CHECK_PLAIN_RET_VAL(rxbio);

	for (i = 0; i < conf->n_units; ++i)
		if (test_bit(Faulty, &conf->units[i].rdev->flags) &&
		    !conf->units[i].redundant &&
		    !conf->units[i].decoding)
			return 0;

	return 1;
}

static unsigned int raidxor_bio_index(raidxor_conf_t *conf,
				      raidxor_bio_t *rxbio,
				      struct bio *bio,
				      unsigned int *data_index)
{
	unsigned int i, k;

	for (i = 0, k = 0; i < rxbio->n_bios; ++i) {
		if (rxbio->bios[i] == bio) {
			if (data_index)
				*data_index = k;
			return i;
		}
		if (!conf->units[i].redundant) ++k;
	}
	CHECK_BUG("didn't find bio");
	return 0;
}

static void raidxor_cache_commit_bio(cache_t *cache, unsigned int n_line)
{
	unsigned int i;
	raidxor_bio_t *rxbio;

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(n_line < cache->n_lines);
	CHECK_PLAIN_RET(cache->lines[n_line]);

	rxbio = cache->lines[n_line]->rxbio;
	CHECK_PLAIN_RET(rxbio);

	for (i = 0; i < rxbio->n_bios; ++i)
		if (!test_bit(Faulty, &cache->conf->units[i].rdev->flags))
			generic_make_request(rxbio->bios[i]);
}

static void raidxor_end_load_line(struct bio *bio, int error);
static void raidxor_end_writeback_line(struct bio *bio, int error);

static int raidxor_cache_load_line(cache_t *cache, unsigned int n_line)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	raidxor_conf_t *conf;
	cache_line_t *line;
	/* sector inside the stripe */
	raidxor_bio_t *rxbio;
	struct bio *bio;
	unsigned int i, j, k, l, n_chunk_mult;
	unsigned long flags = 0;

 	CHECK_FUN(raidxor_cache_load_line);

	CHECK_ARG(cache);
	CHECK_PLAIN(n_line < cache->n_lines);

	conf = cache->conf;
	CHECK_PLAIN(conf);

	line = cache->lines[n_line];

	WITHLOCKCONF(conf, flags, {
	if (line->status == CACHE_LINE_LOAD_ME)
		line->status = CACHE_LINE_LOADING;
	else {
		UNLOCKCONF(conf, flags);
		goto out;
	}
	});

	/* unrecoverable error, abort */
	if (test_bit(CONF_ERROR, &conf->flags)) {
		CHECK_BUG("conf with error code in load_line");
		goto out;
	}

	rxbio = raidxor_alloc_bio(conf->n_units);
	CHECK_PLAIN(rxbio);
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_free_bio

	rxbio->cache = cache;
	rxbio->line = n_line;
	rxbio->remaining = rxbio->n_bios;

	line->rxbio = rxbio;

	n_chunk_mult = cache->n_chunk_mult;

	for (i = 0, l = 0; i < rxbio->n_bios; ++i) {
		/* we also load the redundant pages */

		/* only one chunk */
		rxbio->bios[i] = bio = bio_alloc(GFP_NOIO, n_chunk_mult);
		CHECK_ALLOC(rxbio->bios[i]);

		bio->bi_rw = READ;
		bio->bi_private = rxbio;
		bio->bi_bdev = conf->units[i].rdev->bdev;
		bio->bi_end_io = raidxor_end_load_line;

		bio->bi_sector = line->sector;
		/* bio->bi_sector = actual_sector / stripe->n_data_units + */
		/* 	stripe->units[i].rdev->data_offset; */
		do_div(bio->bi_sector, conf->n_data_units);
		bio->bi_sector += conf->units[i].rdev->data_offset;

		bio->bi_size = n_chunk_mult * PAGE_SIZE;

		/* assign pages */
		bio->bi_vcnt = n_chunk_mult;
		for (j = 0; j < n_chunk_mult; ++j) {
			if (conf->units[i].redundant)
				k = (cache->n_buffers + l) * n_chunk_mult + j;
			else k = i * n_chunk_mult + j;

			CHECK_PLAIN(line->buffers[k]);
			bio->bi_io_vec[j].bv_page = line->buffers[k];

			bio->bi_io_vec[j].bv_len = PAGE_SIZE;
			bio->bi_io_vec[j].bv_offset = 0;
		}

		if (conf->units[i].redundant) ++l;

		if (test_bit(Faulty, &conf->units[i].rdev->flags)) {
			--rxbio->remaining;
			if (!conf->units[i].redundant)
				rxbio->faulty = 1;
		}
	}

	WITHLOCKCONF(conf, flags, {
	++cache->active_lines;
	});

	return 0;
out_free_bio: __attribute__((unused))
	raidxor_free_bio(rxbio);
out: __attribute__((unused))
	raidxor_cache_abort_requests(cache, n_line);
	return 1;
}

static int raidxor_cache_writeback_line(cache_t *cache, unsigned int n_line)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	cache_line_t *line;
	raidxor_bio_t *rxbio;
	unsigned int i, j, k, l, n_chunk_mult;
	struct bio *bio;
	unsigned long flags = 0;
	raidxor_conf_t *conf = cache->conf;

 	CHECK_FUN(raidxor_cache_writeback_line);

	CHECK_ARG(cache);
	CHECK_PLAIN(n_line < cache->n_lines);

	line = cache->lines[n_line];

	WITHLOCKCONF(conf, flags, {
	if (line->status == CACHE_LINE_DIRTY)
		line->status = CACHE_LINE_WRITEBACK;
	else {
		UNLOCKCONF(conf, flags);
		goto out;
	}
	});

	rxbio = raidxor_alloc_bio(conf->n_units);
	CHECK_PLAIN(rxbio);
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_free_bio

	rxbio->cache = cache;
	rxbio->line = n_line;
	rxbio->remaining = rxbio->n_bios;

	line->rxbio = rxbio;

	n_chunk_mult = cache->n_chunk_mult;

	for (i = 0, l = 0; i < rxbio->n_bios; ++i) {
		/* only one chunk */
		rxbio->bios[i] = bio = bio_alloc(GFP_NOIO, cache->n_chunk_mult);
		CHECK_ALLOC(rxbio->bios[i]);

		bio->bi_rw = WRITE;
		bio->bi_private = rxbio;
		bio->bi_bdev = conf->units[i].rdev->bdev;
		bio->bi_end_io = raidxor_end_writeback_line;

		bio->bi_sector = line->sector;
		/* bio->bi_sector = actual_sector / stripe->n_data_units + */
		/* 	stripe->units[i].rdev->data_offset; */
		do_div(bio->bi_sector, conf->n_data_units);
		bio->bi_sector += conf->units[i].rdev->data_offset;

		bio->bi_size = n_chunk_mult * PAGE_SIZE;

		bio->bi_vcnt = n_chunk_mult;
		/* assign pages */
		for (j = 0; j < n_chunk_mult; ++j) {
			if (conf->units[i].redundant)
				k = (cache->n_buffers + l) * n_chunk_mult + j;
			else k = i * n_chunk_mult + j;

			bio->bi_io_vec[j].bv_page = line->buffers[k];
			bio->bi_io_vec[j].bv_len = PAGE_SIZE;
			bio->bi_io_vec[j].bv_offset = 0;
		}

		if (conf->units[i].redundant) ++l;

		if (test_bit(Faulty, &conf->units[i].rdev->flags)) {
			--rxbio->remaining;
			if (!conf->units[i].redundant)
				rxbio->faulty = 1;
		}
	}

	for (i = 0; i < conf->n_enc_temps; ++i) {
		if (raidxor_xor_combine_encode_temporary(cache, n_line,
							 &line->temp_buffers[i * cache->n_chunk_mult],
							 rxbio,
							 conf->enc_temps[i]))
			goto out_free_bio;
	}
	
	for (i = 0; i < rxbio->n_bios; ++i) {
		if (!conf->units[i].redundant) continue;
		if (raidxor_xor_combine_encode(cache, n_line,
					       rxbio->bios[i], rxbio,
					       conf->units[i].encoding))
			goto out_free_bio;
	}

	WITHLOCKCONF(conf, flags, {
	++cache->active_lines;
	});

	return 0;
out_free_bio:
	raidxor_free_bio(rxbio);
out: __attribute__((unused))
	return 1;
}

static void raidxor_end_load_line(struct bio *bio, int error)
{
	raidxor_bio_t *rxbio;
	raidxor_conf_t *conf;
	cache_t *cache;
	cache_line_t *line;
	unsigned int index, data_index, wake = 0;
	unsigned long flags = 0;

	CHECK_FUN(raidxor_end_load_line);

	CHECK_ARG_RET(bio);

	rxbio = (raidxor_bio_t *)(bio->bi_private);
	CHECK_PLAIN_RET(rxbio);

	cache = rxbio->cache;
	CHECK_PLAIN_RET(cache);

	CHECK_PLAIN_RET(rxbio->line < cache->n_lines);

	line = cache->lines[rxbio->line];
	CHECK_PLAIN_RET(line);

	conf = rxbio->cache->conf;
	CHECK_PLAIN_RET(conf);

	index = raidxor_bio_index(conf, rxbio, bio, &data_index);

	if (error) {
		WITHLOCKCONF(conf, flags, {
		if (!conf->units[index].redundant)
			rxbio->faulty = 1;
		});
		md_error(conf->mddev, conf->units[index].rdev);
	}

	WITHLOCKCONF(conf, flags, {
	if ((--rxbio->remaining) == 0) {
		if (rxbio->faulty)
			line->status = CACHE_LINE_FAULTY;
		else  {
			line->status = CACHE_LINE_UPTODATE;
			line->rxbio = NULL;
			raidxor_free_bio(rxbio);
		}
		--cache->active_lines;
		wake = 1;
	}
	});

	if (wake) raidxor_wakeup_thread(conf);
}

static void raidxor_end_writeback_line(struct bio *bio, int error)
{
	raidxor_bio_t *rxbio;
	raidxor_conf_t *conf;
	cache_t *cache;
	cache_line_t *line;
	unsigned int index, data_index, wake = 0;
	unsigned long flags = 0;

	CHECK_FUN(raidxor_end_writeback_line);

	CHECK_ARG_RET(bio);

	rxbio = (raidxor_bio_t *)(bio->bi_private);
	CHECK_PLAIN_RET(rxbio);

	cache = rxbio->cache;
	CHECK_PLAIN_RET(cache);

	CHECK_PLAIN_RET(rxbio->line < cache->n_lines);

	line = cache->lines[rxbio->line];
	CHECK_PLAIN_RET(line);

	conf = rxbio->cache->conf;
	CHECK_PLAIN_RET(conf);

	index = raidxor_bio_index(conf, rxbio, bio, &data_index);

	if (error)
		md_error(conf->mddev, conf->units[index].rdev);

	WITHLOCKCONF(conf, flags, {
	if ((--rxbio->remaining) == 0) {
		line->status = CACHE_LINE_UPTODATE;

		line->rxbio = NULL;
		raidxor_free_bio(rxbio);

		--cache->active_lines;
		wake = 1;
	}
	});

	if (wake) raidxor_wakeup_thread(conf);
}

static int raidxor_xor_combine_temporary(cache_t *cache,
					 unsigned int n_line,
					 struct page **target,
					 raidxor_bio_t *rxbio,
					 unsigned int n_units,
					 coding_t *units,
					 unsigned int encoding)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	unsigned int i, j, k, index;
	unsigned int err __attribute__((unused));
	struct bio *biofrom;
	unsigned char *tomapped;
	struct page **temps;
	unsigned int nsrcs = 0;
	const unsigned int nblocks = 5;
	struct page *pages[nblocks];
	void *srcs[nblocks];
	struct bio *bios[nblocks];
	raidxor_conf_t *conf;

	CHECK_FUN(raidxor_xor_combine_temporary);

	CHECK_ARG(cache);
	CHECK_ARG(rxbio);

	conf = cache->conf;

	i = 1;

	if (units[0].temporary) {
		if (encoding)
			index = raidxor_find_enc_temps(cache->conf, units[0].encoding);
		else
			index = raidxor_find_dec_temps(cache->conf, units[0].decoding);
		temps = &cache->lines[n_line]->temp_buffers[index * cache->n_chunk_mult];
		raidxor_copy_pages(cache->n_chunk_mult, target, temps);
	}
	else {
		/* copying first bio buffers */
		biofrom = raidxor_find_bio(rxbio, units[0].disk);
		raidxor_copy_bio_to_pages(target, biofrom);
	}

	/* XOR every NBLOCKS bio_vecs, repeating for all bio_vec of the bios */
	while (i < n_units) {
		for (j = 0; j < cache->n_chunk_mult; ++j) {
			tomapped = (unsigned char *) kmap(target[j]);

			for (k = i, nsrcs = 0; k < n_units && k < (i + nblocks); ++k, ++nsrcs) {
				if (units[k].temporary) {
					if (encoding)
						index = raidxor_find_enc_temps(cache->conf, units[k].encoding);
					else
						index = raidxor_find_dec_temps(cache->conf, units[k].decoding);
					pages[nsrcs] = cache->lines[n_line]->temp_buffers[cache->n_chunk_mult * index + j];
				}
				else {
					if (j == 0) bios[nsrcs] = raidxor_find_bio(rxbio, units[k].disk);
					pages[nsrcs] = bio_iovec_idx(bios[nsrcs], j)->bv_page;
				}
				srcs[nsrcs] = kmap(pages[nsrcs]);
			}

			xor_blocks(nsrcs, PAGE_SIZE, tomapped, srcs);

			for (k = 0; k < nsrcs; ++k)
				kunmap(pages[k]);
			kunmap(target[j]);
		}
		i += nsrcs;
	}

	return 0;
out: __attribute((unused))
	return 1;
}


static int raidxor_xor_combine_encode_temporary(cache_t *cache, unsigned int n_line,
						struct page **pages,
						raidxor_bio_t *rxbio,
						encoding_t *encoding)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_ARG(cache)
	CHECK_ARGS3(pages, rxbio, encoding);

	return raidxor_xor_combine_temporary(cache, n_line, pages, rxbio,
					     encoding->n_units,
					     encoding->units, 1);
out: __attribute((unused))
	return 1;
}

static int raidxor_xor_combine_decode_temporary(cache_t *cache, unsigned int n_line,
						struct page **pages,
						raidxor_bio_t *rxbio,
						decoding_t *decoding)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_ARG(cache)
	CHECK_ARGS3(pages, rxbio, decoding);

	return raidxor_xor_combine_temporary(cache, n_line, pages, rxbio,
					     decoding->n_units,
					     decoding->units, 0);
out: __attribute((unused))
	return 1;
}

static int raidxor_xor_combine(cache_t *cache, unsigned int n_line,
			       struct bio *bioto,
			       raidxor_bio_t *rxbio,
			       unsigned int n_units, coding_t *units,
			       unsigned int encoding)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	unsigned int i, j, k, index;
	struct bio *biofrom;
	struct bio_vec *bvto;
	unsigned char *tomapped;
	struct page **temps;
	unsigned int nsrcs = 0;
	const unsigned int nblocks = 5;
	struct page *pages[nblocks];
	void *srcs[nblocks];
	struct bio *bios[nblocks];
	raidxor_conf_t *conf;

	CHECK_FUN(raidxor_xor_combine);

	CHECK_ARG(units);

	conf = cache->conf;

	i = 1;

	if (units[0].temporary) {
		if (encoding)
			index = raidxor_find_enc_temps(cache->conf, units[0].encoding);
		else
			index = raidxor_find_dec_temps(cache->conf, units[0].decoding);
		temps = &cache->lines[n_line]->temp_buffers[index * cache->n_chunk_mult];
		raidxor_copy_pages_to_bio(bioto, temps);
	}
	else {
		/* copying first bio buffers */
		biofrom = raidxor_find_bio(rxbio, units[0].disk);
		raidxor_copy_bio(bioto, biofrom);
	}

	/* XOR every NBLOCKS bio_vecs, repeating for all bio_vec of the bios */
	while (i < n_units) {
		for (j = 0; j < bioto->bi_vcnt; ++j) {
			bvto = bio_iovec_idx(bioto, j);
			tomapped = (unsigned char *) kmap(bvto->bv_page);

			for (k = i, nsrcs = 0; k < n_units && k < (i + nblocks); ++k, ++nsrcs) {
				if (units[k].temporary) {
					if (encoding)
						index = raidxor_find_enc_temps(cache->conf, units[k].encoding);
					else
						index = raidxor_find_dec_temps(cache->conf, units[k].decoding);
					pages[nsrcs] = cache->lines[n_line]->temp_buffers[cache->n_chunk_mult * index + j];
				}
				else {
					if (j == 0) bios[nsrcs] = raidxor_find_bio(rxbio, units[k].disk);
					pages[nsrcs] = bio_iovec_idx(bios[nsrcs], j)->bv_page;
				}
				srcs[nsrcs] = kmap(pages[nsrcs]);
			}

			xor_blocks(nsrcs, PAGE_SIZE, tomapped, srcs);

			for (k = 0; k < nsrcs; ++k)
				kunmap(pages[k]);
			kunmap(bvto->bv_page);
		}
		i += nsrcs;
	}

	return 0;
out: __attribute((unused))
	return 1;
}

static int raidxor_xor_combine_decode(cache_t *cache, unsigned int n_line,
				      struct bio *bioto,
				      raidxor_bio_t *rxbio,
				      decoding_t *decoding)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_ARG(cache)
	CHECK_ARGS3(bioto, rxbio, decoding);

	return raidxor_xor_combine(cache, n_line, bioto, rxbio, decoding->n_units, decoding->units, 0);
out: __attribute((unused))
	return 1;
}

/**
 * raidxor_xor_combine_encode() - xors a number of resources together
 *
 * Takes a master request and combines the request inside the rxbio together
 * using the given encoding for the unit.
 *
 * Returns 1 on error (bioto still might be touched in this case).
 */
static int raidxor_xor_combine_encode(cache_t *cache, unsigned int n_line,
				      struct bio *bioto,
				      raidxor_bio_t *rxbio,
				      encoding_t *encoding)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_ARG(cache);
	CHECK_ARGS3(bioto, rxbio, encoding);

	return raidxor_xor_combine(cache, n_line, bioto, rxbio, encoding->n_units, encoding->units, 1);
out: __attribute((unused))
	return 1;
}

/**
 * raidxor_cache_recover() - tries to recover a cache line
 *
 * Since the read buffers are available, we can use them to calculate
 * the missing data.
 */
static void raidxor_cache_recover(cache_t *cache, unsigned int n_line)
{
	cache_line_t *line;
	raidxor_conf_t *conf;
	raidxor_bio_t *rxbio;
	unsigned int i;
	unsigned long flags = 0;

	CHECK_FUN(raidxor_cache_recover);

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET(line);

	rxbio = line->rxbio;
	CHECK_PLAIN_RET(rxbio);

	conf = cache->conf;
	CHECK_PLAIN_RET(conf);

	WITHLOCKCONF(conf, flags, {
	if (!raidxor_valid_decoding(cache, n_line))
		goto out_free_rxbio_unlock;

	line->status = CACHE_LINE_RECOVERY;
	});

	/* decoding temporaries first */
	for (i = 0; i < conf->n_dec_temps; ++i) {
		if (raidxor_xor_combine_decode_temporary(cache, n_line,
							 &line->temp_buffers[i * cache->n_chunk_mult],
							 rxbio,
							 conf->dec_temps[i]))
			goto out_free_rxbio;
	}

	/* decoding using direct style */
	for (i = 0; i < rxbio->n_bios; ++i) {
		if (test_bit(Faulty, &conf->units[i].rdev->flags)) {
			if (conf->units[i].redundant) {
				if (raidxor_xor_combine_encode(cache, n_line,
							       rxbio->bios[i], rxbio,
							       conf->units[i].encoding))
					goto out_free_rxbio;
			}
			else {
				if (raidxor_xor_combine_decode(cache, n_line,
							       rxbio->bios[i], rxbio,
							       conf->units[i].decoding))
					goto out_free_rxbio;
			}
		}
	}

	WITHLOCKCONF(conf, flags, {
	line->rxbio = NULL;
	line->status = CACHE_LINE_UPTODATE;
	});

	raidxor_free_bio(rxbio);

	return;
out_free_rxbio_unlock:
	UNLOCKCONF(conf, flags);
out_free_rxbio:
	line->rxbio = NULL;
	raidxor_free_bio(rxbio);
	/* drop this line if an error occurs or we can't recover */

	raidxor_cache_abort_requests(cache, n_line);

	LOCKCONF(conf, flags);
	line->status = CACHE_LINE_READY;
	UNLOCKCONF(conf, flags);
}

static void raidxor_invalidate_decoding(raidxor_conf_t *conf,
					disk_info_t *unit)
{
	unsigned int i;

	CHECK_ARG_RET(conf);
	CHECK_ARG_RET(unit);

	if (unit->decoding) {
		CHECK_BUG("unit has decoding, although it shouldn't have one");
		raidxor_safe_free_decoding(unit);
	}

	for (i = 0; i < conf->n_units; ++i)
		if (conf->units[i].decoding &&
		    raidxor_find_unit_decoding(conf->units[i].decoding,
					       unit)) {
			raidxor_safe_free_decoding(&conf->units[i]);
		}

	printk(KERN_CRIT "raidxor: raid %s needs new decoding information\n",
	       mdname(conf->mddev));
}

/**
 * raidxor_error() - propagates a device error
 *
 */
static void raidxor_error(mddev_t *mddev, mdk_rdev_t *rdev)
{
	unsigned long flags = 0;
	char buffer[BDEVNAME_SIZE];
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	disk_info_t *unit = raidxor_find_unit_conf_rdev(conf, rdev);

	WITHLOCKCONF(conf, flags, {
	if (!test_bit(Faulty, &rdev->flags)) {
		/* escalate error */
		set_bit(Faulty, &rdev->flags);
		set_bit(CONF_FAULTY, &conf->flags);
		raidxor_invalidate_decoding(conf, unit);
		printk(KERN_CRIT "raidxor: disk failure on %s\n",
		       bdevname(rdev->bdev, buffer));
	}
	});
}

/**
 * raidxor_finish_lines() - tries to free some lines by writeback or dropping
 *
 */
static void raidxor_finish_lines(cache_t *cache)
{
	unsigned int i;
	cache_line_t *line;
	unsigned int freed = 0;
	unsigned long flags = 0;

	CHECK_FUN(raidxor_finish_lines);

	CHECK_ARG_RET(cache);

	WITHLOCKCONF(cache->conf, flags, {
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_PLAIN(cache->n_waiting > 0);
	CHECK_PLAIN(cache->n_lines > 0);

	/* as long as there are more waiting slots than now free'd slots */
	for (i = 0; i < cache->n_lines && freed < cache->n_waiting; ++i) {
		line = cache->lines[i];
		switch (line->status) {
		case CACHE_LINE_CLEAN:
			if (!line->waiting) ++freed;
		case CACHE_LINE_READY:
			break;
		case CACHE_LINE_UPTODATE:
			if (line->waiting) break;
			UNLOCKCONF(cache->conf, flags);
			raidxor_cache_make_ready(cache, i);
			LOCKCONF(cache->conf, flags);
			++freed;
			break;
		case CACHE_LINE_DIRTY:
			if (line->waiting) break;
			/* when the callback is invoked, the main thread is
			   woken up and eventually revisits this entry  */
			UNLOCKCONF(cache->conf, flags);
			if (!raidxor_cache_writeback_line(cache, i)) {
				raidxor_cache_commit_bio(cache, i);
			}
			LOCKCONF(cache->conf, flags);
			break;
		case CACHE_LINE_LOAD_ME:
		case CACHE_LINE_LOADING:
		case CACHE_LINE_WRITEBACK:
		case CACHE_LINE_FAULTY:
		case CACHE_LINE_RECOVERY:
		case CACHE_LINE_READYING:
			/* can't do anything useful with these */
			break;
			/* there is no default */
		}
	}

out: __attribute__((unused))
	do {} while (0);

	});

	for (i = 0; i < freed; ++i)
		raidxor_signal_empty_line(cache->conf);
}

/**
 * raidxor_handle_requests() - handles waiting requests for a cache line
 *
 *
 */
static void raidxor_handle_requests(cache_t *cache, unsigned int n_line)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	cache_line_t *line;
	struct bio *bio;
	unsigned long flags = 0;

	CHECK_FUN(raidxor_handle_requests);

	CHECK_ARG(cache);
	CHECK_PLAIN(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN(line);

	WITHLOCKCONF(cache->conf, flags, {
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_unlock
	CHECK_PLAIN(line->status == CACHE_LINE_UPTODATE ||
		    line->status == CACHE_LINE_DIRTY);

	/* requests are added at back, so take from front and handle */
	while ((bio = raidxor_cache_remove_request(cache, n_line))) {
		UNLOCKCONF(cache->conf, flags);

		if (bio_data_dir(bio) == WRITE)
			raidxor_copy_bio_to_cache(cache, n_line, bio);
		else raidxor_copy_bio_from_cache(cache, n_line, bio);

		bio_endio(bio, 0);

		LOCKCONF(cache->conf, flags);
		/* mark dirty */
		if (bio_data_dir(bio) == WRITE &&
		    line->status == CACHE_LINE_UPTODATE)
		{
			line->status = CACHE_LINE_DIRTY;
		}
	}
	});

	return;
out_unlock: __attribute((unused))
	UNLOCKCONF(cache->conf, flags);
out: __attribute((unused))
	return;
}

/**
 * raidxor_handle_line() - tries to do something with a cache line
 *
 * Returns 1 when we've done something, else 0.  Errors count as
 * 'done nothing' to prevent endless looping in those cases.
 */
static int raidxor_handle_line(cache_t *cache, unsigned int n_line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	cache_line_t *line;
	unsigned long flags = 0;
	unsigned int commit = 0, done = 0;

	CHECK_FUN(raidxor_handle_line);

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET_VAL(line);

	WITHLOCKCONF(cache->conf, flags, {

	/* if nobody wants something from this line, do nothing */
	if (!line->waiting) goto out_unlock;

	switch (line->status) {
	case CACHE_LINE_LOAD_ME:
		UNLOCKCONF(cache->conf, flags);
		commit = !raidxor_cache_load_line(cache, n_line);
		done = 1;
		goto break_unlocked;
	case CACHE_LINE_FAULTY:
		UNLOCKCONF(cache->conf, flags);
		raidxor_cache_recover(cache, n_line);
		done = 1;
		goto break_unlocked;
	case CACHE_LINE_UPTODATE:
	case CACHE_LINE_DIRTY:
		UNLOCKCONF(cache->conf, flags);
		raidxor_handle_requests(cache, n_line);
		done = 1;
		goto break_unlocked;
	case CACHE_LINE_READY:
	case CACHE_LINE_RECOVERY:
	case CACHE_LINE_LOADING:
	case CACHE_LINE_WRITEBACK:
	case CACHE_LINE_CLEAN:
	case CACHE_LINE_READYING:
		/* no bugs, just can't do anything */
		break;
		/* no default */
	}

	});
break_unlocked:

	if (commit) raidxor_cache_commit_bio(cache, n_line);

	return done;
out_unlock:
	UNLOCKCONF(cache->conf, flags);

	return 0;
}

/**
 * raidxord() - daemon thread
 *
 * Is started by the md level.  Takes requests from the queue and handles them.
 */
static void raidxord(mddev_t *mddev)
{
	unsigned int i;
	raidxor_conf_t *conf;
	cache_t *cache;
	unsigned int handled = 0;
	unsigned int done = 0;
	unsigned long flags = 0;

	CHECK_ARG_RET(mddev);

	conf = mddev_to_conf(mddev);
	CHECK_PLAIN_RET(conf);

	WITHLOCKCONF(conf, flags, {
	done = test_bit(CONF_INCOMPLETE, &conf->flags);
	});
	if (done) return;

	cache = conf->cache;
	CHECK_PLAIN_RET(cache);

	/* someone poked us.  see what we can do */
	pr_debug("raidxor: raidxord active\n");

	for (; !done;) {
		/* go through all cache lines, see if any waiting requests
		   can be handled */
		for (i = 0, done = 1; i < cache->n_lines; ++i) {
			/* only break if we have handled at least one line */
			if (raidxor_handle_line(cache, i)) {
				++handled;
				done = 0;
			}
		}

		/* also, if somebody is waiting for a free line, try to make
		   one (or more) available.  freeing some lines doesn't count
		   for done above, so if we're done working on those lines
		   and we free two lines afterwards, the waiting processes
		   are notified and signal us back later on */

		if (cache->n_waiting > 0) raidxor_finish_lines(cache);
	}

	pr_debug("raidxor: thread inactive, %u lines handled\n", handled);
}

static void raidxor_unplug(struct request_queue *q)
{
	mddev_t *mddev = q->queuedata;
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned int i;
	struct request_queue *r_queue;

	for (i = 0; i < conf->n_units; i++) {
		r_queue = bdev_get_queue(conf->units[i].rdev->bdev);

		blk_unplug(r_queue);
	}
}

/**
 * raidxor_run() - basic initialization for the raid
 *
 * We can't use it after this, because the layout of the raid is not
 * described yet.  Therefore, every read/write operation fails until
 * we've got enough information.
 */
static int raidxor_run(mddev_t *mddev)
{
	raidxor_conf_t *conf;
	struct list_head *tmp;
	mdk_rdev_t* rdev;
	char buffer[32];
	sector_t size;
	unsigned long i;

	if (mddev->level != LEVEL_XOR) {
		printk(KERN_ERR "raidxor: %s: raid level not set to xor (%d)\n",
		       mdname(mddev), mddev->level);
		goto out_inval;
	}

	if (mddev->chunk_size < PAGE_SIZE) {
		printk(KERN_ERR "raidxor: chunk_size must be at least "
		       "PAGE_SIZE but %d < %ld\n",
		       mddev->chunk_size, PAGE_SIZE);
		goto out_inval;
	}

	printk(KERN_INFO "raidxor: raid set %s active with %d disks\n",
	       mdname(mddev), mddev->raid_disks);

	if (mddev->raid_disks < 1)
		goto out_inval;

	conf = kzalloc(sizeof(raidxor_conf_t) +
		       sizeof(struct disk_info) * mddev->raid_disks, GFP_KERNEL);
	mddev->private = conf;
	if (!conf) {
		printk(KERN_ERR "raidxor: couldn't allocate memory for %s\n",
		       mdname(mddev));
		goto out;
	}

	set_bit(CONF_INCOMPLETE, &conf->flags);
	conf->mddev = mddev;
	conf->chunk_size = mddev->chunk_size;
	conf->units_per_resource = 0;
	conf->n_resources = 0;
	conf->resources = NULL;
	conf->n_units = mddev->raid_disks;

	blk_queue_hardsect_size(mddev->queue, 4096);

	spin_lock_init(&conf->device_lock);
	mddev->queue->queue_lock = &conf->device_lock;
	mddev->queue->unplug_fn = raidxor_unplug;

	size = -1; /* rdev->size is in sectors, that is 1024 byte */

	i = conf->n_units - 1;
	rdev_for_each(rdev, tmp, mddev) {
		size = min(size, rdev->size);

		printk(KERN_INFO "raidxor: device %lu rdev %s, %llu blocks\n",
		       i, bdevname(rdev->bdev, buffer),
		       (unsigned long long) rdev->size * 2);
		conf->units[i].rdev = rdev;
		conf->units[i].redundant = -1;

		--i;
	}
	if (size == -1)
		goto out_free_conf;

	/* used component size in sectors, multiple of chunk_size ... */
	mddev->size = size & ~(conf->chunk_size / 1024 - 1);
	/* exported size in blocks, will be initialised later */
	mddev->array_sectors = 0;

	/* Ok, everything is just fine now */
	if (sysfs_create_group(&mddev->kobj, &raidxor_attrs_group)) {
		printk(KERN_ERR
		       "raidxor: failed to create sysfs attributes for %s\n",
		       mdname(mddev));
		goto out_free_conf;
	}

	mddev->thread = md_register_thread(raidxord, mddev, "%s_raidxor");
	if (!mddev->thread) {
		printk(KERN_ERR
		       "raidxor: couldn't allocate thread for %s\n",
		       mdname(mddev));
		goto out_free_sysfs;
	}

	return 0;

out_free_sysfs:
	sysfs_remove_group(&mddev->kobj, &raidxor_attrs_group);

out_free_conf:
	if (conf) {
		kfree(conf);
		mddev_to_conf(mddev) = NULL;
	}
out:
	return -EIO;

out_inval:
	return -EINVAL;
}

static int raidxor_stop(mddev_t *mddev)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned long flags = 0;

	WITHLOCKCONF(conf, flags, {
	set_bit(CONF_STOPPING, &conf->flags);
	raidxor_wait_for_no_active_lines(conf, &flags);
	raidxor_wait_for_writeback(conf, &flags);
	});

	md_unregister_thread(mddev->thread);
	mddev->thread = NULL;

	sysfs_remove_group(&mddev->kobj, &raidxor_attrs_group);
	blk_sync_queue(mddev->queue);

	mddev_to_conf(mddev) = NULL;
	raidxor_safe_free_conf(conf);
	raidxor_complete_free_conf(conf);
	kfree(conf);

	return 0;
}

static void raidxor_align_sector_to_strip(raidxor_conf_t *conf,
					  sector_t *sector)
{
	sector_t strip_sectors;
	sector_t div;
	sector_t mod;

	CHECK_ARG_RET(conf);
	CHECK_ARG_RET(sector);

	strip_sectors = (conf->chunk_size >> 9) * conf->n_data_units;

	/* mod = *sector % strip_sectors; */
	div = *sector;
	mod = do_div(div, strip_sectors);

	if (mod != 0)
		*sector -= mod;
}

static int raidxor_check_bio_size_and_layout(raidxor_conf_t *, struct bio *) __attribute__((unused));
/**
 * raidxor_check_bio_size_and_layout() - checks a bio for compatibility
 *
 * Checks whether the size is a multiple of PAGE_SIZE and each bio_vec
 * is exactly one page long and has an offset of 0.
 */
static int raidxor_check_bio_size_and_layout(raidxor_conf_t *conf,
					     struct bio *bio)
{
	unsigned int i;
	struct bio_vec *bvl;
	sector_t div, mod;

	div = bio->bi_size;
	mod = do_div(div, PAGE_SIZE);

	if (mod != 0)
		return 1;

	bio_for_each_segment(bvl, bio, i) {
		if (bvl->bv_len != PAGE_SIZE)
			return 2;

		if (bvl->bv_offset != 0)
			return 3;
	}			

	return 0;
}

static int raidxor_make_request(struct request_queue *q, struct bio *bio)
{
	mddev_t *mddev;
	raidxor_conf_t *conf;
	cache_t *cache;
	unsigned int line;
	sector_t aligned_sector, strip_sectors, mod, div;
	unsigned long flags = 0;

#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_ARG(q);
	CHECK_ARG(bio);

	mddev = q->queuedata;
	CHECK_PLAIN(mddev);

	conf = mddev_to_conf(mddev);
	CHECK_PLAIN(conf);

	cache = conf->cache;
	CHECK_PLAIN(cache);

	WITHLOCKCONF(conf, flags, {
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_unlock

	if (test_bit(CONF_STOPPING, &conf->flags) ||
	    test_bit(CONF_ERROR, &conf->flags))
		goto out_unlock;

	CHECK_PLAIN(!raidxor_check_bio_size_and_layout(conf, bio));

	strip_sectors = (conf->chunk_size >> 9) * conf->n_data_units;

	aligned_sector = bio->bi_sector;

	/* round sector down to current or previous strip */
	raidxor_align_sector_to_strip(conf, &aligned_sector);

	/* set as offset to new base */
	bio->bi_sector = bio->bi_sector - aligned_sector;

	/* checked assumption is: aligned_sector is aligned to
	   strip/cache line, bio->bi_sector is the offset inside this strip
	   (and aligned to PAGE_SIZE) */

	div = aligned_sector;
	mod = do_div(div, PAGE_SIZE >> 9);
	CHECK_PLAIN(mod == 0);

	div = aligned_sector;
	mod = do_div(div, strip_sectors);
	CHECK_PLAIN(mod == 0);

	div = bio->bi_sector;
	mod = do_div(div, PAGE_SIZE >> 9);
	CHECK_PLAIN(mod == 0);

	if (bio->bi_sector + (bio->bi_size >> 9) > strip_sectors) {
		printk(KERN_ERR "need to split request because "
		       "%llu > %llu\n",
		       (unsigned long long) (bio->bi_sector +
					     (bio->bi_size >> 9)),
		       (unsigned long long) strip_sectors);
		goto out_unlock;
	}

retry:
	/* look for matching line or otherwise available */
	if (!raidxor_cache_find_line(cache, aligned_sector, &line)) {
		raidxor_wait_for_empty_line(conf, &flags);

		if (test_bit(CONF_STOPPING, &conf->flags) ||
		    test_bit(CONF_ERROR, &conf->flags)) {
			goto out_unlock;
		}
	}

	if (!raidxor_cache_find_line(cache, aligned_sector, &line)) {
		printk(KERN_ERR "couldn't find available line\n");
		goto out_unlock;
	}

	if (cache->lines[line]->status == CACHE_LINE_CLEAN ||
	    cache->lines[line]->status == CACHE_LINE_READY)
	{
		UNLOCKCONF(conf, flags);
		if (raidxor_cache_make_ready(cache, line))
			goto out_retry_lock;
		LOCKCONF(conf, flags);

		if (cache->lines[line]->status != CACHE_LINE_READY)
			goto out_retry;

		if (raidxor_cache_make_load_me(cache, line, aligned_sector)) {
			printk(KERN_ERR "raidxor_cache_make_load_me failed mysteriously\n");
			goto out_unlock;
		}
	}

	/* pack the request somewhere in the cache */
	raidxor_cache_add_request(cache, line, bio);
	});

	raidxor_wakeup_thread(conf);

	return 0;
out_retry_lock:
	LOCKCONF(conf, flags);
out_retry:
	goto retry;
out_unlock:
	UNLOCKCONF(conf, flags);
out: __attribute__((unused))
	bio_io_error(bio);
	return 0;
}

#include "init.c"

#if 0
Local variables:
c-basic-offset: 8
End:
#endif
