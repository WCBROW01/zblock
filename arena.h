/* This arena allocator implementation was created by Will Brown (WCBROW01).
 * Orginal source can be found at: https://github.com/WCBROW01/arena-allocator
 * Licensed under the MIT License (c) 2022-2024 Will Brown */

#ifndef ARENA_H
#define ARENA_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Arena Arena;

// Allocates a fixed-size arena. Accepts the size of the arena in bytes.
Arena *Arena_new(size_t size);

/* Allocates a dynamically-sized arena. Accepts the initial size of the arena in bytes.
 * If there is not enough space in the arena for an allocation, a new region will be created. */
Arena *Arena_new_dynamic(size_t size);

// Frees the entire arena from memory.
void Arena_delete(Arena *arena);

// Will return a null pointer if you've tried allocating too much memory.
void *Arena_alloc(Arena *arena, size_t size);

// Identical to Arena_alloc but it zeros your memory
void *Arena_allocz(Arena *arena, size_t size);

/* Copy a block of memory into an arena.
 * Functionally equivalent to memcpy. */
void *Arena_copy(Arena *arena, const void *src, size_t size);

/* If the pointer is to the last allocation, it will be resized.
 * Otherwise, a new allocation will be created.
 * Be careful with this! A null pointer will be returned upon error.
 * Using this with memory outside of the arena is undefined behavior. */
void *Arena_realloc(Arena *arena, void *ptr, size_t size);

/* Marks the beginning of a temporary buffer that can be deallocated at any time.
 * The state of the last one is saved in case you have multiple. */
void Arena_tmp_begin(Arena *arena);

/* Deallocates the last temporary buffer. If there is none,
 * the entire arena will be deallocated. */
void Arena_tmp_rewind(Arena *arena);

#ifdef __cplusplus
}
#endif

#endif
