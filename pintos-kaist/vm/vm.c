/* vm.c: 가상 메모리 객체를 위한 일반 인터페이스. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "kernel/hash.h"
#include "threads/mmu.h"

static struct list frame_table;			// 할당된 모든 frame을 보관해 둘 리스트
static struct lock frame_table_lock;	// frame_table 접근 시 동시성 문제 방지를 위한 락

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
	// ※ 위의 라인은 수정하지마세요. ※
	/* TODO: Your code goes here. */
	// frame_table 초기화 함수 추가
	list_init(&frame_table);
	lock_init(&frame_table_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
// 페이지 유형을 가져옵니다. 이 함수는 페이지가 초기화된 후 페이지의 유형을 알고 싶을 때 유용합니다. 
// 이 함수는 현재 완전히 구현되었습니다.
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
// 초기화 함수를 사용하여 보류 중인 페이지 객체를 생성합니다. 
// 페이지를 생성하려면 직접 생성하지 말고 이 함수나 `vm_alloc_page`를 통해 생성하세요.
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{
	ASSERT(VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current()->spt;

	/* Check wheter the upage is already occupied or not. */

	if (spt_find_page(spt, upage) == NULL)
	{
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		// 해야될 것: 페이지를 생성하고, VM 유형에 따라 초기화 파일을 가져온 후, uninit_new를 호출하여 "uninit" 페이지 구조체를 생성합니다.
		// 해야될 것: uninit_new를 호출한 후 필드를 수정해야 합니다.

		/* TODO: Insert the page into the spt. */
		// 해당 페이지를 spt에 삽입합니다.
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
// spt에서 VA를 찾아 페이지를 반환합니다. 오류가 발생하면 NULL을 반환합니다.
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
// 내보낼 구조체 프레임을 가져옵니다.
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */
	if(list_empty(&frame_table))
		return NULL;
	 // 내보내는 규칙은 본인이 정하세요.

	 struct list_elem *e;
	// fifo 전략 - 가장 먼저 올라온 (가장 오래된) 페이지부터 교체
	// e = list_pop_front(&frame_table);
	// victim = list_entry(e,struct frame, frame_elem);


	//LRU (Least Recently Used) 가장 오랬동안 사용되지 않은 페이지부터 교체
	/* CLOCK 방식 */
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
// 한 페이지를 제거하고 해당 프레임을 반환
// 오류 발생 시 NULL을 반환합니다.
static struct frame *
vm_evict_frame (void) {
	lock_acquire(&frame_table_lock);
	struct frame *victim = vm_get_victim ();
	lock_release(&frame_table_lock);
	/* TODO: swap out the victim and return the evicted frame. */
	// 희생(victim) 페이지를 스왑 아웃하고, 제거된 프레임을 반환합니다.
	if(victim ==NULL) 
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
// palloc() 함수는 프레임을 가져옵니다. 사용 가능한 페이지가 없으면 해당 페이지를 제거하고 반환합니다.
// 이 함수는 항상 유효한 주소를 반환합니다. 
// 즉, 사용자 풀 메모리가 가득 차면 이 함수는 프레임을 제거하여 사용 가능한 메모리 공간을 가져옵니다.
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
		frame->kva = kva;
		frame->page = NULL;
		
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
// 스택을 키웁니다.
static void
vm_stack_growth(void *addr UNUSED)
{
}

/* Handle the fault on write_protected page */
// write_protected 페이지에서 오류를 처리합니다.
static bool
vm_handle_wp(struct page *page UNUSED)
{
}

/* Return true on success */

bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED,
						 bool user UNUSED, bool write UNUSED, bool not_present UNUSED)
{
	struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	// 오류를 검증하세요
	/* TODO: Your code goes here */
	// 코드를 여기에 적으세요

	return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
// 페이지를 비웁니다. 
// ※ 해당 함수는 수정하지 마세요! ※
void
vm_dealloc_page (struct page *page)
{
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
// VA로 할당된 페이지를 선언합니다.
/* 역할 : 1. “SPT에 해당 가상주소가 없으면 새로 만들어서 삽입한 뒤, 물리 프레임을 연결”하는 것
		 2. SPT에 존재하면 그거 가져와서 vm_do_claim으로 매핑하기 */ 
bool
vm_claim_page (void *va) 
{
	struct page *page = spt_find_page(thread_current()->spt,va);
	/* TODO: Fill this function */
	// 기능을 구현하세요.
	if(page == NULL){
		page = vm_alloc_page(VM_ANON,va,true);	// va에 쓰기 가능 익명 페이지를 나타내는 새 struct page를 할당하여 반환한다
		if(page == NULL) return false;
	}
	spt_insert_page(thread_current()->spt,page);

	page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
		return false;

	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
// PAGE를 선언하고 mmu를 설정하세요.
// 가상페이지를 물리 메모리와 연결해서 사용자 코드가 그 가상 주소를 정상적으로 접근하게 하는 함수 
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
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
								  struct supplemental_page_table *src)
{
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt)
{
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */

	struct hash_iterator i;
	struct page *p;

	/* 전체 해쉬 테이블 순회하면서 vm_dealloc_page 수행*/
	hash_first(&i, &spt->spt_hash);
	while (hash_next(&i))
	{
		p = hash_entry(hash_cur(&i), struct page, hash_elem);

 		spt_remove_page(spt,p);		//해당 페이지를 spt에서 제거 하고 메모리 까지 해제
	}

	// 버킷 배열 메모리 자체를 해제
	hash_destroy(&spt->spt_hash, NULL);
}
