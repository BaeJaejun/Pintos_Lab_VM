/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <string.h>

#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/mmu.h"

/* user‐mode 페이지를 담고 있는 모든 frame을 추적할 리스트 */
static struct list frame_table;
/* frame_list에 대한 동기화를 위한 락 */
static struct lock frame_table_lock;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
	vm_anon_init();
	vm_file_init();
#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	// frame_table 초기화 함수 추가
	list_init(&frame_table);
	lock_init(&frame_table_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{

	ASSERT(VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current()->spt;

	/* va가 페이지 경계로 정렬되어 있는지 확인 */
	void *va = pg_round_down(upage);

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page(spt, va) == NULL)
	{
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		struct page *page = (struct page *)malloc(sizeof *page);
		if (page == NULL)
			return false;

		/* uninit_page 구조체 내부 함수 포인터를 결정 */
		bool (*page_initializer)(struct page *, enum vm_type, void *kva);

		switch (VM_TYPE(type))
		{
		case VM_ANON:
			page_initializer = anon_initializer;
			break;
		case VM_FILE:
			page_initializer = file_backed_initializer;
			break;
		default:
			free(page);
			return false;
		}
		uninit_new(page, va, init, type, aux, page_initializer);

		page->writable = writable;

		/* TODO: Insert the page into the spt. */
		if (!spt_insert_page(spt, page))
		{
			free(page);
			return false;
		}
		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page(struct supplemental_page_table *spt, void *va)
{
	struct page *page = NULL;
	/* TODO: Fill this function. */
	struct hash_elem *he;
	struct page tmp; // 검색용 임시 페이지 변수
	/* va를 페이지 경계(시작위치)로 내림(round down) */
	tmp.va = pg_round_down(va);

	he = hash_find(&spt->spt_hash, &tmp.hash_elem);
	if (he == NULL)
		return NULL;

	page = hash_entry(he, struct page, hash_elem);
	return page;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page)
{
	int succ = false;
	/* TODO: Fill this function. */
	// hash_insert()는 성공 시 null을 반환, 이미 같은 키가 있으면 기존의 hash_elem 반환
	struct hash_elem *he = hash_insert(&spt->spt_hash, &page->hash_elem);
	if (he == NULL)
		succ = true;
	return succ;
}

bool spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	/* hash_delete 추가 */
	struct hash_elem *he = hash_delete(&spt->spt_hash, &page->hash_elem);
	if (he == NULL)
		return false;

	vm_dealloc_page(page);

	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim(void)
{
	// struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */
	if (list_empty(&frame_table))
		return NULL;

	/* FIFO 방식 */
	// struct list_elem *e = list_pop_front(&frame_table);
	// victim = list_entry(e, struct frame, frame_elem);

	/* CLOCK 방식 */
	struct list_elem *e;
	struct frame *victim;
	for (e = list_begin(&frame_table); e != list_end(&frame_table);)
	{
		victim = list_entry(e, struct frame, frame_elem);

		if (pml4_is_accessed(thread_current()->pml4, victim->page->va))
		{
			/* 참조되었으니, second chance: accessed 비트만 0으로 내리고 뒤로 보낸다 */
			pml4_set_accessed(thread_current()->pml4, victim->page->va, false);
			e = list_remove(e); // list_next를 반환해줌
			list_push_back(&frame_table, &victim->frame_elem);
		}
		else
		{
			/* accessed 비트가 이미 0 → 이게 바로 victim */
			list_remove(e);
			return victim;
		}
	}

	/* 2) 위 루프에서 victim을 못 찾았다면, 리스트 앞에서 그냥 꺼낸다 (가장 오래된 것) */
	e = list_pop_front(&frame_table);
	victim = list_entry(e, struct frame, frame_elem);
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame(void)
{
	lock_acquire(&frame_table_lock);
	struct frame *victim = vm_get_victim();
	lock_release(&frame_table_lock);
	/* TODO: swap out the victim and return the evicted frame. */
	if (victim == NULL)
		return NULL;

	/* victim이 차지하고 있는 페이지가 있다면 swap_out*/
	if (!swap_out(victim->page))
		return NULL;

	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame(void)
{

	struct frame *frame = NULL;
	/* TODO: Fill this function. */

	/* 1) 빈 유저 페이지가 있는지 할당 시도 <- 빈 물리 페이지를 할당*/
	void *kva = palloc_get_page(PAL_USER);
	if (kva != NULL)
	{
		/* 받아온 kva를 관리할 struct frame을 malloc <- 프레임 테이블 위함 */
		frame = (struct frame *)malloc(sizeof(struct frame));
		if (frame == NULL)
		{
			palloc_free_page(kva);
			return NULL;
		}
		/* 새 프레임 내부 필드 초기화 */
		frame->kva = kva;	/* 실제 물리 페이지의 커널 가상 주소 */
		frame->page = NULL; /* 아직 어떤 SPTE와도 매핑되지 않은 상태 */

		lock_acquire(&frame_table_lock);
		list_push_back(&frame_table, &frame->frame_elem);
		lock_release(&frame_table_lock);

		ASSERT(frame != NULL);
		ASSERT(frame->page == NULL);

		return frame;
	}

	/* 2) 빈 유저 페이지가 없을 때 evicit(축출) 수행 */
	struct frame *victim = vm_evict_frame();
	if (victim == NULL)
	{
		/* evicition 실패 했다면 NULL을 리턴
			함수 상단 주석을 보면 항상 옳은 주소 반환 -> 실패 없음
		*/
		return NULL;
	}
	victim->page = NULL;

	ASSERT(victim != NULL);
	ASSERT(victim->page == NULL);

	return victim;
}

/* Growing the stack. */
static void
vm_stack_growth(void *addr UNUSED)
{
	void *upage = pg_round_down(addr);
	vm_alloc_page(VM_ANON, upage, true);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp(struct page *page UNUSED)
{
}

/* Return true on success */
// bool vm_try_handle_fault(struct intr_frame *f, void *addr,
// 						 bool user, bool write, bool not_present)
// {
// 	struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
// 	struct page *page = NULL;
// 	uintptr_t sp = is_kernel_vaddr(f->rsp) ? thread_current()->rsp_stack : f->rsp; // f-> rsp는 유저 모드에서 페이지 폴트가 난 시점의 사용자 스택 포인터 값
// 	/* fault된 가상 주소를 페이지 경계로 내림 */
// 	void *fault_page = pg_round_down(addr);

// 	/* TODO: Validate the fault */
// 	/* TODO: Your code goes here */

// 	// /* spt에 예약된 uninit 페이지가 있으면 물리 메모리로 올리기 */
// 	// page = spt_find_page(spt, fault_page);
// 	// if (page != NULL)
// 	// 	return vm_do_claim_page(page);
// 	// else
// 	// {
// 	// 	/* todo: 스택 확장? */
// 	// 	if (!is_user_vaddr(addr) || !not_present)
// 	// 		return false;
// 	// 	if (!(((uintptr_t)fault_page < sp) && (sp - (uintptr_t)fault_page <= 32)))
// 	// 		return false;
// 	// 	if ((USER_STACK - (uintptr_t)fault_page) >= 1024 * 1024)
// 	// 		return false;
// 	// 	vm_stack_growth(fault_page);
// 	// 	page = spt_find_page(spt, fault_page);
// 	// 	if (page != NULL)
// 	// 	{
// 	// 		vm_do_claim_page(page);
// 	// 		return true;
// 	// 	}
// 	// }

// 	// return false;

// 	if (!is_user_vaddr(addr) || !not_present)
// 		return false;

// 	page = spt_find_page(spt, fault_page);
// 	if (page)
// 		return vm_do_claim_page(page);
// 	if (((uintptr_t)fault_page < sp) && ((sp - (uintptr_t)fault_page) <= 32) && (((uintptr_t)thread_current()->stack_bottom - (uintptr_t)fault_page) < (1024 * 1024)))
// 	{
// 		vm_stack_growth(thread_current()->stack_bottom - PGSIZE);
// 		page = spt_find_page(spt, fault_page);
// 		if (page)
// 			return vm_do_claim_page(page);
// 	}
// 	return false;
// }
/*-------------------------------------------------------------------------------------------------------------------------------------------*/

// bool vm_try_handle_fault(struct intr_frame *f, void *addr,
// 						 bool user, bool write, bool not_present)
// {
// 	struct thread *t = thread_current();
// 	struct supplemental_page_table *spt = &t->spt;
// 	void *fault_page = pg_round_down(addr);

// 	/* 1) 유저 영역 확인 & 권한 오류 차단 */
// 	if (!is_user_vaddr(addr) || !not_present)
// 		return false;

// 	/* 2) rsp_stack + sp 분리 */
// 	void *rsp_stack = is_kernel_vaddr(f->rsp)
// 						  ? t->rsp_stack
// 						  : f->rsp;
// 	uintptr_t sp = (uintptr_t)rsp_stack;

// 	/* 3) SPT 검색 → 이미 있으면 페이지 클레임 */
// 	struct page *page = spt_find_page(spt, fault_page);
// 	if (page)
// 		return vm_do_claim_page(page);

// 	/* 4) 스택 확장 조건:
// 	 *    1) fault_page < sp
// 	 *    2) sp - fault_page ≤ 32
// 	 *    3) (현재 바닥 - fault_page) ≤ 허용 최대 크기
// 	 */
// 	uintptr_t cur_bottom = (uintptr_t)t->stack_bottom;
// 	const size_t MAX_STACK = 256 * PGSIZE; // 예: 1MiB
// 	if ((uintptr_t)fault_page < sp && sp - (uintptr_t)fault_page <= 32 && cur_bottom - (uintptr_t)fault_page <= MAX_STACK)
// 	{
// 		/* 5) 정확한 위치로 스택 확장 호출 */
// 		vm_stack_growth(t->stack_bottom - PGSIZE);

// 		/* 6) 다시 SPT 검색 & 클레임 */
// 		page = spt_find_page(spt, fault_page);
// 		if (page)
// 			return vm_do_claim_page(page);
// 	}

// 	return false;
// }

/*-------------------------------------------------------------------------------------------------------------------------------------------*/
bool vm_try_handle_fault(struct intr_frame *f, void *addr,
						 bool user, bool write, bool not_present)
{
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page *page = NULL;

	if (is_kernel_vaddr(addr))
		return false;
	void *rsp_stack = f->rsp;
	if (not_present)
	{
		if (!vm_claim_page(addr))
		{
			if (rsp_stack - 8 <= addr && USER_STACK - 0x100000 <= addr && addr <= USER_STACK)
			{
				vm_stack_growth(thread_current()->stack_bottom - PGSIZE);
				return true;
			}
			return false;
		}
		else
			return true;
	}
	return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va)
{
	struct page *page = NULL;
	/* TODO: Fill this function */
	/* 현재 스레드의 spt에서 해당 VA에 할당된 struct page 찾기 */

	// spt_find_page 실패시 오류처리
	page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
		return false;

	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* 가상 주소와 물리 주소를 매핑 */
	pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
	return swap_in(page, frame->kva);
}

/* 해시 함수: page->va 주소 자체를 바이트 배열로 보고 해싱
	pintos에서 제공해주는 해시 함수 hash_bytes() 사용
*/
static unsigned page_hash(const struct hash_elem *e, void *aux UNUSED)
{
	struct page *p = hash_entry(e, struct page, hash_elem);

	return hash_bytes(&p->va, sizeof(p->va));
}

/* 비교 함수: 두 page의 va 값을 포인터 크기 기준으로 비교 후 bool 값 리턴 */
static bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
	struct page *pa = hash_entry(a, struct page, hash_elem);
	struct page *pb = hash_entry(b, struct page, hash_elem);

	return pa->va < pb->va;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt)
{
	// 해시 테이블 초기화
	hash_init(&spt->spt_hash, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
/* 부모 프로세스의 spt(src)를 복사하여 자식 프로세스 spt(dst)에 붙여 넣는 함수 */
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
								  struct supplemental_page_table *src)
{
	struct hash_iterator i;
	struct page *p;
	bool succ;

	/* 부모의 해쉬 테이블 순회하면서 page별 상태에 따라 복사하기 */
	hash_first(&i, &src->spt_hash);
	while (hash_next(&i))
	{
		p = hash_entry(hash_cur(&i), struct page, hash_elem);
		if (p == NULL)
			return false;
		/* 아직 물리 할당 없이 예약만 된 페이지인 경우(lazy_loading중인 페이지인 경우)  */
		if (p->operations->type == VM_UNINIT)
		{
			if (!vm_alloc_page_with_initializer(p->uninit.type, p->va, p->writable,
												p->uninit.init, p->uninit.aux))
				return false;
		}
		/* anon이나 file_back일 경우  */
		else
		{
			/* 자식 SPT에 페이지 객체 생성 */
			if (!vm_alloc_page(p->operations->type, p->va, p->writable))
				return false;
			/* 물리 페이지 할당 및 초기화 (파일 읽기/스왑 복원/zero‐fill) */
			if (!vm_claim_page(p->va))
				return false;

			/* 부모의 물리 메모리 내용을 복사하기  */
			struct page *child_p = spt_find_page(dst, p->va);
			memcpy(child_p->frame->kva, p->frame->kva, PGSIZE);
		}
	}
	return true;
}
/* 페이지 타입에 맞게 삭제해주는 함수  */
static void page_destroy(struct hash_elem *e, void *aux UNUSED)
{
	struct page *p = hash_entry(e, struct page, hash_elem);
	/* 이 한 줄로 프레임 해제, 스왑 슬롯 반환, aux free, 그리고 free(p)까지 수행 */
	vm_dealloc_page(p);
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt)
{
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	/* TODO: writeback all the modified contents to the storage. */

	/* 버킷 배열 메모리 자체를 해제 */
	hash_clear(&spt->spt_hash, page_destroy); // hash_clear 안에서 순회를 한다.
}
