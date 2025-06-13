/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void)
{
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

static bool
lazy_load_mmap(struct page *page, void *aux)
{

	struct file_page *info = (struct file_page *)aux;
	struct file *file = info->file;
	off_t offset = info->offset;
	uint32_t read_bytes = info->read_bytes;
	uint32_t zero_bytes = info->zero_bytes;

	/* 1) 이미 페이지에 매핑된 frame을 준비함 */
	struct frame *frame = page->frame;
	if (frame == NULL)
	{
		free(info);
		return false;
	}
	void *kva = frame->kva;

	/* 2) 파일 위치 설정 후, read_bytes만큼 읽어서 kva에 복사 */
	file_seek(file, offset);
	if (file_read(file, kva, read_bytes) != (int)read_bytes)
	{
		free(info);
		return false;
	}

	/* 3) 나머지 부분(zero_bytes)만큼 0으로 채움 */
	memset(kva + read_bytes, 0, zero_bytes);

	// free(info);
	return true;
}

/* Swap in the page by read contents from the file. */
/* file_backed_swap_in():
   파일 기반 페이지의 데이터를 디스크(파일 시스템)에서 물리 페이지(kva)로 읽어와
   해당 프레임을 완전히 채우는 역할만 수행하는 함수.
   — 파일 I/O 오류 검사
   — 파일에서 읽어온 부분 뒤의 영역을 0으로 초기화
   (페이지 테이블 매핑 및 TLB 업데이트는 vm_do_claim_page() 등 상위 로직에서 처리) */
static bool
file_backed_swap_in(struct page *page, void *kva)
{
	struct file_page *file_page UNUSED = &page->file;

	/* 디스크(파일)에서 read_bytes 만큼 읽어와 kva에 저장 */
	off_t size = file_read_at(file_page, kva, file_page->read_bytes, file_page->offset);

	/* 읽기 성공 여부 검사 */
	if (size != file_page->read_bytes)
		return false;

	/* 남은 부분(zero_bytes)을 0으로 채워 페이지를 완전 초기화 */
	memset(kva + file_page->read_bytes, 0, file_page->zero_bytes);

	return true;
}

/* Swap out the page by writeback contents to the file. */
/* 파일 기반 페이지가 수정된 경우 해당 페이지의 내용을 원본 파일에 기록하고
	페이지를 언매핑하여 다음 접근 시 다시 로드되도록 준비하는 함수 */
static bool
file_backed_swap_out(struct page *page)
{
	struct file_page *file_page UNUSED = &page->file;
	/* dirty 비트를 검사하여 해당 페이지가 수정된 상태인지 확인/ 수정되지 않았다면 바로 return true */
	if (pml4_is_dirty(thread_current()->pml4, page->va))
	{
		/* 수정된 페이지에 한에서 파일에 변경 내용을 기록한다. */
		file_write_at(file_page, page->va, file_page->read_bytes, file_page->offset);
		/* 그 후 dirty비트를 초기화 한다*/
		pml4_set_dirty(thread_current()->pml4, page->va, 0);
	}
	/*해당 가상주소와 물리프레임 간의 매핑을 완전히 해제하고,
	이후 그 주소에 접근할 때 반드시 페이지 폴트를 발생시켜 VM 서브시스템이 다시 적절한 처리를(스왑인·lazy load 등) 하도록 “강제”하기 위함 */
	pml4_clear_page(thread_current()->pml4, page->va);

	return true;
}
/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy(struct page *page)
{
	// struct file_page *file_page UNUSED = &page->file;
	struct file_page *file_page UNUSED = &page->file;

	if (pml4_is_dirty(thread_current()->pml4, page->va))
	{
		file_write_at(file_page->file, file_page->start_addr, file_page->read_bytes, file_page->offset);
		pml4_set_dirty(thread_current()->pml4, page->va, false);
	}

	if (page->frame)
	{
		list_remove(&page->frame->frame_elem);
		page->frame->page = NULL;
		free(page->frame);
		page->frame = NULL;
	}

	pml4_clear_page(thread_current()->pml4, page->va);
	hash_delete(&thread_current()->spt.spt_hash, &page->hash_elem);
}

/* Do the mmap */
/* 유저가 요청한 파일 구간을 가상주소 공간에 매핑하기 위해 SPT에 예약해주는 함수*/
void *
do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset)
{
	size_t page_cnt = (length + PGSIZE - 1) / PGSIZE;

	/* 인자 유효성 검사 */
	if (addr == NULL || pg_ofs(addr) != 0 || !is_user_vaddr(addr))
		return NULL;
	if (is_kernel_vaddr(addr + (page_cnt * PGSIZE) - 1)) // mmap-kernel 테스트
		return NULL;
	if (length == 0)
		return NULL;
	if (file == NULL || file_length(file) == 0)
		return NULL;
	if (offset < 0 || file_length(file) < offset || offset % PGSIZE != 0)
		return NULL;

	/* 나중에 do_mmap 성공했을때 반환할 시작 주소 저장*/
	void *start_addr = addr;

	addr = pg_round_down(addr);

	/* 주어진 파일 길이와 length를 비교해서 length보다 file 크기가 작으면 파일 통으로 싣고 파일 길이가 더 크면 주어진 length만큼만 load*/
	size_t read_bytes = length > file_length(file) ? file_length(file) : length;
	size_t zero_bytes = PGSIZE - read_bytes % PGSIZE; // 마지막 페이지에 들어갈 자투리 바이트

	/* 파일 복제 */
	struct file *file_cp = file_reopen(file);
	if (!file_cp)
		return NULL;

	/* 페이지 단위로 SPT 예약 */
	while (read_bytes > 0 || zero_bytes > 0)
	{
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 겹침 검사 */
		if (spt_find_page(&thread_current()->spt, addr))
			return NULL;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		/*  aux 할당 및 초기화와
			로드를 위해 필요한 정보를 가진 file_page 구조체 만들었음*/
		struct file_page *aux = (struct file_page *)malloc(sizeof(*aux));
		if (aux == NULL)
			return NULL;

		aux->file = file_cp;
		aux->offset = offset;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->writable = writable;

		aux->start_addr = start_addr;
		aux->length = length;

		/* SPT 예약: lazy_load_segment 으로 나중에 실제 로드 */
		if (!vm_alloc_page_with_initializer(VM_FILE, addr,
											writable, lazy_load_mmap, aux))
		{
			free(aux);
			file_close(file_cp);
			return NULL;
		}

		/* Advance. */
		/* 다음 페이지 정보 갱신 */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		addr += PGSIZE;
		/* 파일에서 연속적으로 다음 페이지를 읽어 오기 위함 */
		offset += page_read_bytes; /* ofs를 읽어들인 만큼만 증가 */
	}
	return start_addr;
}

/* Do the munmap */
void do_munmap(void *addr)
{
	struct thread *cur = thread_current();

	addr = pg_round_down(addr);

	/* 첫 페이지에서 file 핸들 꺼내기 */
	struct page *page = spt_find_page(&cur->spt, addr);
	if (!page)
		return;
	struct file_page *first_aux = (struct file_page *)page->uninit.aux;
	struct file *file = first_aux->file;

	while (true)
	{
		/* 매핑 정보 조회*/
		struct page *page = spt_find_page(&thread_current()->spt, addr);
		if (page == NULL)
			break;

		struct file_page *aux = (struct file_page *)page->uninit.aux;

		/* 수정된 페이지(dirty bit == 1)는 파일에 업데이트해놓는다. 이후에 dirty bit을 0으로 만든다. */
		if (pml4_is_dirty(thread_current()->pml4, page->va)) //	pml4_is_dirty함수는 페이지의 dirty bit이 1이면 true를, 0이면 false를 리턴한다.
		{
			/* 물리 프레임에 변경된 데이터를 다시 디스크 파일에 업데이트해주는 함수. buffer에 있는 데이터를 size만큼, file의 file_ofs부터 써준다 */
			file_write_at(aux->file, page->frame->kva, aux->read_bytes, aux->offset);
			/* 인자로 받은 dirty의 값이 1이면 page의 dirty bit을 1로, 0이면 0으로 변경해준다. */
			pml4_set_dirty(thread_current()->pml4, page->va, 0);
		}
		/* pml4 페이지 안에서 va와 매핑된거 지우는 함수 */
		pml4_clear_page(thread_current()->pml4, page->va);

		/* c) physical frame 해제 */
		// if (page->frame)
		// {
		// 	palloc_free_page(page->frame->kva);
		// 	free(page->frame);
		// }

		/* d) SPT 엔트리 & aux 해제 */
		// spt_remove_page(&cur->spt, page);
		// free(aux);

		addr += PGSIZE;
	}
	/* 4) 파일 닫기 */
	file_close(file);
}
