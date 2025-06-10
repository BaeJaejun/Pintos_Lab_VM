#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page
{
	struct file *file;	 /* file_reopen() 으로 얻은 파일 핸들 */
	off_t offset;		 /* 이 페이지가 파일에서 읽어올 시작 오프셋 */
	uint32_t read_bytes; /* 이 페이지에 실제로 읽어들일 바이트 수 */
	uint32_t zero_bytes; /* 페이지의 나머지 부분(읽을 데이터 이후)을 0으로 채울 바이트 수 */
	bool writable;		 /* 쓰기 권한 */
};

void vm_file_init(void);
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
			  struct file *file, off_t offset);
void do_munmap(void *va);
#endif
