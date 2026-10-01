/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Audio.
 *
 * Ghoti.io Audio is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Audio is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * cutil's allocator, for the cross-architecture build only.
 *
 * `make check-golden` compiles this library for big-endian targets and runs
 * it under qemu. Cross-building cutil as well would be the thorough thing
 * and it is not what this gate is for: the question is whether *this*
 * library's bytes-to-samples path answers the same on a machine of the
 * other endianness, and cutil's allocator is not on that path - it hands
 * back memory.
 *
 * So the target build gets cutil's headers, which is where the type and the
 * symbol renaming live, and these four definitions instead of its library.
 * The names below come out namespaced exactly as cutil's own do, because
 * the header renames them before it declares them.
 *
 * `font` makes the same trade for the same reason; its cross_shim.c says so
 * in its own words.
 */

#include <ghoti.io/cutil/allocator.h>
#include <stdlib.h>
#include <string.h>

static void * shim_malloc(void * ctx, size_t size) {
  (void)ctx;
  /* A zero-size request returns a usable non-NULL pointer, so that NULL
   * always means failure - which is what the allocator contract in
   * allocator.h requires and what this library's error paths assume. */
  return malloc(size ? size : 1);
}

static void * shim_calloc(void * ctx, size_t nitems, size_t size) {
  (void)ctx;
  if (nitems && size > (size_t)-1 / nitems) {
    return NULL; /* overflow is an allocation failure, not a small block */
  }
  size_t total = nitems * size;
  return calloc(total ? total : 1, 1);
}

static void * shim_realloc(void * ctx, void * ptr, size_t size) {
  (void)ctx;
  return realloc(ptr, size ? size : 1);
}

static void shim_free(void * ctx, void * ptr) {
  (void)ctx;
  free(ptr);
}

static const GCU_Allocator shim_allocator = {
    .malloc_fn = shim_malloc,
    .calloc_fn = shim_calloc,
    .realloc_fn = shim_realloc,
    .free_fn = shim_free,
    .ctx = NULL,
};

const GCU_Allocator * gcu_allocator_default(void) {
  return &shim_allocator;
}

void * gcu_allocator_malloc(const GCU_Allocator * allocator, size_t size) {
  const GCU_Allocator * a = allocator ? allocator : &shim_allocator;
  return a->malloc_fn(a->ctx, size);
}

void * gcu_allocator_calloc(
    const GCU_Allocator * allocator, size_t nitems, size_t size) {
  const GCU_Allocator * a = allocator ? allocator : &shim_allocator;
  return a->calloc_fn(a->ctx, nitems, size);
}

void * gcu_allocator_realloc(
    const GCU_Allocator * allocator, void * ptr, size_t size) {
  const GCU_Allocator * a = allocator ? allocator : &shim_allocator;
  return a->realloc_fn(a->ctx, ptr, size);
}

void gcu_allocator_free(const GCU_Allocator * allocator, void * ptr) {
  const GCU_Allocator * a = allocator ? allocator : &shim_allocator;
  a->free_fn(a->ctx, ptr);
}

/*
 * security's gsec_wipe, for the cross-architecture build only.
 *
 * Phase 4 made `security` a dependency, for one function: FLAC's
 * STREAMINFO carries an MD5 of the unencoded audio. That digest **is** on
 * the path this gate measures - it goes into a file this library writes,
 * and §11.1's promise covers what we write as well as what we read - so
 * unlike cutil's allocator it cannot be stubbed with something that
 * returns a plausible answer. The real md5.c is compiled for the target
 * instead, and this is the one function it needs that lives elsewhere.
 *
 * Volatile through the pointer, so that a compiler cannot decide the
 * stores are dead and remove them. That is the whole content of a secure
 * wipe and the reason it is not a plain memset.
 */

#include <ghoti.io/security/secret.h>

GSEC_Result gsec_wipe(void * p, size_t n) {
  if (!p && n) {
    return GSEC_ERR_INVALID;
  }
  volatile unsigned char * at = (volatile unsigned char *)p;
  while (n--) {
    *at++ = 0;
  }
  return GSEC_OK;
}
