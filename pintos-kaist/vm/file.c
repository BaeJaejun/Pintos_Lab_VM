/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
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

	/* aux 구조체로 캐스트 */
	struct file_page *aux = (struct file_page *)kva;
	if (aux == NULL)
		return false;

	/* aux의 정보를 page->file 에 채워넣기 */
	page->file.file = aux->file;
	page->file.ofs = aux->ofs;
	page->file.read_bytes = aux->read_bytes;
	page->file.zero_bytes = aux->zero_bytes;

	/* 페이지가 쓰기 가능해야 할 경우 */
	page->writable = aux->writable;

	/* uninit 단계 aux는 더 이상 필요 없으니 해제 */
	// free(aux);

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in(struct page *page, void *kva)
{
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out(struct page *page)
{
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy(struct page *page)
{
	struct file_page *file_page = &page->file;

	/* dirty 시 write-back */
	if (pml4_is_dirty(thread_current()->pml4, page->va))
	{
		file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
		pml4_set_dirty(thread_current()->pml4, page->va, 0);
	}
	pml4_clear_page(thread_current()->pml4, page->va);
}

/* Do the mmap */
void *
do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset)
{

	off_t total_len = file_length(file);
	uint32_t read_bytes = total_len - offset;
	uint32_t zero_bytes = (PGSIZE - (read_bytes % PGSIZE)) % PGSIZE;

	off_t ofs = offset;
	uint8_t *upage = (uint8_t *)addr;

	/* 기본 검증 */
	if (addr == 0 || length == 0 || file == NULL || offset < 0 || pg_ofs(addr) != 0 /* 페이지 경계인지 검사 */
		|| !is_user_vaddr(addr) || total_len == 0 || offset > total_len)
		return NULL;

	/* 매핑 메타정보 생성·등록 */
	struct mmap_info *mi = malloc(sizeof *mi);
	if (!mi)
		return NULL;
	mi->start_addr = addr;
	mi->page_cnt = (length + PGSIZE - 1) / PGSIZE;
	mi->file = file_reopen(file);
	mi->writable = writable;
	list_push_front(&thread_current()->mmap_list, &mi->elem);

	/* load_segmet와 비슷함 */
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct file_page *aux = (struct file_page *)malloc(sizeof(*aux));
		if (aux == NULL)
		{
			do_munmap(addr);
			return NULL;
		}
		aux->file = mi->file;
		aux->ofs = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->writable = writable;

		if (!vm_alloc_page_with_initializer(VM_FILE, upage,
											writable, file_backed_initializer, aux))
		{
			free(aux);
			do_munmap(addr);
			return NULL;
		}

		/* Advance. */
		/* 4) 다음 반복을 위해 각 값 갱신 */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		/* 파일에서 연속적으로 다음 페이지를 읽어 오기 위함 */
		ofs += page_read_bytes; /* ofs를 읽어들인 만큼만 증가 */
	}
	return addr;
}

/* Do the munmap */
void do_munmap(void *addr)
{
	struct thread *cur = thread_current();
	struct list_elem *e, *next;
	struct mmap_info *mi = NULL;
	/* 1) mmap_info 찾기 */
	for (e = list_begin(&cur->mmap_list); e != list_end(&cur->mmap_list); e = list_next(e))
	{
		struct mmap_info *m = list_entry(e, struct mmap_info, elem);
		if (m->start_addr == addr)
		{
			mi = m;
			break;
		}
	}
	if (mi == NULL)
		return; /* 해당 주소로 매핑된 정보가 없으면 아무 것도 하지 않음 */

	/* 2) 매핑된 각 페이지 언매핑 */
	for (int i = 0; i < mi->page_cnt; i++)
	{
		void *upage = mi->start_addr + i * PGSIZE;
		struct page *p = spt_find_page(&cur->spt, upage);
		if (p == NULL)
			continue;

		/* swap_out 내부에서 dirty 체크 및 write-back, 프레임 해제 */
		swap_out(p);

		/* 페이지 구조체도 완전 제거 */
		destroy(p);
	}

	/* 3) 파일 핸들 닫기 */
	// file_close(mi->file);

	/* 4) mmap_info 리스트에서 제거 및 해제 */
	list_remove(&mi->elem);
	free(mi);
}
