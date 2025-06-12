/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include "kernel/bitmap.h"
#include "threads/mmu.h"

/* 익명 페이지 스왑용 디스크와 슬롯 관리 */
struct bitmap *swap_table;								   // 스왑 디스크의 각 슬롯(페이지 단위) 사용 여부를 관리하는 비트맵(비트 하나가 스왑슬롯 하나를 의미하며 0 : 비어있음, 1 : 사용중)
struct lock swap_lock;									   // 스왑 테이블 접근 시 동기화용 락
const size_t SECTORS_PER_PAGE = PGSIZE / DISK_SECTOR_SIZE; // 한 페이지를 저장하기 위해 필요한 디스크 섹터 수 계산

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk; // 스왑 디스크 핸들
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void vm_anon_init(void)
{
	/* TODO: Set up the swap_disk. */
	/* 스왑 디스크 가져오기 */
	swap_disk = disk_get(1, 1);
	if (swap_disk == NULL)
		PANIC("vm_anon_init: swap disk not found");

	/* 스왑 슬롯 수 계산*/
	size_t swap_size = disk_size(swap_disk) / SECTORS_PER_PAGE;

	/* 스왑 슬롯 관리용 비트맵 생성 -> 비트맵 초기화까지 다 되어있음  */
	swap_table = bitmap_create(swap_size); // BIT_CNT 비트 크기의 비트맵으로 초기화하고 비트맵 생성하기

	/* 락 초기화 */
	lock_init(&swap_lock);
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &anon_ops; // 익명페이지 전용 page_op 설정

	struct anon_page *anon_page = &page->anon;

	/* 스왑 슬롯 초기화 */
	anon_page->swap_slot = -1;

	/* 페이지 메모리 0으로 초기화 */
	memset(kva, 0, PGSIZE);

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in(struct page *page, void *kva)
{
	struct anon_page *anon_page = &page->anon;

	/* 스왑 슬롯 번호 확인 */
	int slot_number = page->anon.swap_slot;
	/* slot_number가 -1이면 스왑된적이 없다. 즉, 복훤할 데이터가 없다. */
	if (slot_number < 0)
		return false;

	///* slot number가 -1이면 스왑된 적이 없다. 즉 , 복원할 데이터가 없다 */
	// if (bitmap_test(swap_disk,slot_number) == false)
	//	return false;

	/* 디스크에서 메모리로 읽기 */
	for (int i = 0; i < SECTORS_PER_PAGE; i++)
	{
		disk_read(swap_disk, slot_number * SECTORS_PER_PAGE + i, kva + (DISK_SECTOR_SIZE * i));
	}

	/* 4) 스왑 슬롯 해제 및 메타데이터 초기화 */
	bitmap_set(swap_table, anon_page->swap_slot, false); // 슬롯을 빈 상태로 되돌리고
	page->anon.swap_slot = -1;							 // swap_slot 필드를 초기화

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out(struct page *page)
{
	struct anon_page *anon_page = &page->anon;

	/* swap_table 비트맵을 순회해서 아직 사용되지 않은(0인) 슬롯을 찾아서 1로 표시 */
	int slot = bitmap_scan_and_flip(swap_table, 0, 1, false);

	/* 만약 빈 슬롯이 없다면 실패(false)를 반환 */
	if (slot == BITMAP_ERROR)
		return false;

	/*  “slot”번째 스왑 슬롯이 차지하는 디스크 내 첫 섹터 번호 계산
   - 한 슬롯은 SECTORS_PER_PAGE(페이지 크기/섹터 크기)개의 연속된 섹터로 구성
   - 슬롯 0 → 섹터 0~7, 슬롯 1 → 섹터 8~15, … */
	disk_sector_t start_sector = slot * SECTORS_PER_PAGE;

	/* 스왑 슬롯(디스크 영역)에 페이지 내용을 기록
	   - 페이지 크기(PGSIZE)를 섹터 크기(DISK_SECTOR_SIZE) 단위로 분할해
		 한 섹터씩 디스크에 써 넣는다 */
	for (int i = 0; i < SECTORS_PER_PAGE; i++)
	{
		/* start_sector + i : 슬롯 N이 차지하는 첫 섹터 인덱스에서 i만큼 더해 슬롯 내 각 섹터를 순회
		   (uint8_t *)va + i * DISK_SECTOR_SIZE : 페이지의 시작 주소에서 i * 512B만큼 떨어진 버퍼를 가리켜, 한 섹터씩 디스크에 기록 */
		disk_write(swap_disk, start_sector + i, (uint8_t *)page->va + (i * DISK_SECTOR_SIZE));
	}

	page->anon.swap_slot = slot; // 스왑 슬롯 번호 저장(나중에 swap_in 할때 어느 슬롯에서 가져와야 하는지 알아야하기 때문에 저장해줘야한다.)
	/* 페이지 구조체 갱신 -> 물리페이지 free는 안한다! swap_in 할때 재사용 할거임  */
	// palloc_free_page(page->frame->kva); // 물리 페이지 반환
	// free(page->frame);					// frame 구조체 메모리 해제
	// page->frame = NULL;					// 메모리에 없음 표시

	/*  페이지 테이블 업데이트
		해당 가상주소(page->va)의 PTE 중 Present 비트를 0으로 바꿔 주고, 내부적으로 TLB 무효화도 처리해준다.
		따라서 이후 이 주소에 접근하면 페이지 폴트가 발생하게 됨 */
	pml4_clear_page(thread_current()->pml4, page->va);

	/* 성공 반환 */
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy(struct page *page)
{
	// struct anon_page *anon_page = &page->anon;

	// /* 물리 메모리에 올라와 있는 페이지가 있으면 해제 */
	// if (page->frame != NULL)
	// {
	// 	palloc_free_page(page->frame->page);
	// 	page->frame = NULL;
	// }
}
