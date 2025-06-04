#ifndef __LIB_KERNEL_HASH_H
#define __LIB_KERNEL_HASH_H

/* Hash table.
 *
 * This data structure is thoroughly documented in the Tour of
 * Pintos for Project 3.
 *
 * This is a standard hash table with chaining.  To locate an
 * element in the table, we compute a hash function over the
 * element's data and use that as an index into an array of
 * doubly linked lists, then linearly search the list.
 *
 * The chain lists do not use dynamic allocation.  Instead, each
 * structure that can potentially be in a hash must embed a
 * struct hash_elem member.  All of the hash functions operate on
 * these `struct hash_elem's.  The hash_entry macro allows
 * conversion from a struct hash_elem back to a structure object
 * that contains it.  This is the same technique used in the
 * linked list implementation.  Refer to lib/kernel/list.h for a
 * detailed explanation. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "list.h"

/* Hash element. */
struct hash_elem {
	struct list_elem list_elem;
};

/* Converts pointer to hash element HASH_ELEM into a pointer to
 * the structure that HASH_ELEM is embedded inside.  Supply the
 * name of the outer structure STRUCT and the member name MEMBER
 * of the hash element.  See the big comment at the top of the
 * file for an example. */
/* 해시 요소 HASH_ELEM에 대한 포인터를, HASH_ELEM이 포함되어 있는 외부 구조체 STRUCT로 변환합니다.
외부 구조체의 이름 STRUCT와 해시 요소의 멤버 이름 MEMBER를 함께 전달하세요.*/
#define hash_entry(HASH_ELEM, STRUCT, MEMBER)                   \
	((STRUCT *) ((uint8_t *) &(HASH_ELEM)->list_elem        \
		- offsetof (STRUCT, MEMBER.list_elem)))

/* Computes and returns the hash value for hash element E, given
 * auxiliary data AUX. */
/* 보조 데이터 AUX가 주어진 상태에서 해시 요소 E의 해시 값을 계산하여 반환합니다.*/
typedef uint64_t hash_hash_func (const struct hash_elem *e, void *aux);

/* Compares the value of two hash elements A and B, given
 * auxiliary data AUX.  Returns true if A is less than B, or
 * false if A is greater than or equal to B. */
/* 보조 데이터 AUX가 주어진 상태에서 두 해시 요소 A와 B의 값을 비교합니다.
 A가 B보다 작으면 true를 반환하고, 그렇지 않거나 크거나 같으면 false를 반환합니다.*/
typedef bool hash_less_func (const struct hash_elem *a,
		const struct hash_elem *b,
		void *aux);

/* Performs some operation on hash element E, given auxiliary
 * data AUX. */
/* 해시 요소 E에 대해 특정 작업을 수행합니다. AUX는 보조 데이터로,
 * 사용자 정의 작업 함수에서 필요한 추가 정보를 전달합니다. */
typedef void hash_action_func (struct hash_elem *e, void *aux);

/* Hash table. */
struct hash {
	size_t elem_cnt;            /* Number of elements in table. */
	size_t bucket_cnt;          /* Number of buckets, a power of 2. */
	struct list *buckets;       /* Array of `bucket_cnt' lists. */
	hash_hash_func *hash;       /* Hash function. */
	hash_less_func *less;       /* Comparison function. */
	void *aux;                  /* Auxiliary data for `hash' and `less'. */
};

/* A hash table iterator. */
struct hash_iterator {
	struct hash *hash;          /* The hash table. */
	struct list *bucket;        /* Current bucket. */
	struct hash_elem *elem;     /* Current hash element in current bucket. */
};

/* Basic life cycle. */
bool hash_init (struct hash *, hash_hash_func *, hash_less_func *, void *aux);			/* 해시 테이블을 초기화합니다. */
void hash_clear (struct hash *, hash_action_func *);			/* 해시 테이블의 모든 요소에 대해 action 함수를 호출한 후, 해시 테이블을 초기화 상태로 되돌립니다. */
void hash_destroy (struct hash *, hash_action_func *);			/* 해시 테이블을 파괴하며, 저장된 모든 요소에 대해 action 함수를 호출합니다. */

/* Search, insertion, deletion. */
/* 테이블에 elem을 삽입합니다. 이미 동일한 키가 존재하면 삽입하지 않고 기존 요소를 반환합니다. */
struct hash_elem *hash_insert (struct hash *, struct hash_elem *);
/* 테이블에 elem을 삽입하거나, 동일한 키가 이미 존재하면 기존 요소를 새로운 elem으로 교체하여 반환합니다. */
struct hash_elem *hash_replace (struct hash *, struct hash_elem *);
/* 테이블에서 elem과 동일한 키를 가진 요소를 찾습니다. 찾으면 해당 요소를 반환하고, 없으면 NULL을 반환합니다. */
struct hash_elem *hash_find (struct hash *, struct hash_elem *);
/* 테이블에서 elem과 동일한 키를 가진 요소를 삭제하고 반환합니다. 삭제할 요소가 없으면 NULL을 반환합니다. */
struct hash_elem *hash_delete (struct hash *, struct hash_elem *);

/* Iteration. */
void hash_apply (struct hash *, hash_action_func *);
void hash_first (struct hash_iterator *, struct hash *);
struct hash_elem *hash_next (struct hash_iterator *);
struct hash_elem *hash_cur (struct hash_iterator *);

/* Information. */
size_t hash_size (struct hash *);
bool hash_empty (struct hash *);

/* Sample hash functions. */
uint64_t hash_bytes (const void *, size_t);
uint64_t hash_string (const char *);
uint64_t hash_int (int);

#endif /* lib/kernel/hash.h */
