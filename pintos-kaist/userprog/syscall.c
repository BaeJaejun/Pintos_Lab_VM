#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

/* syscall 위한 헤더 선언 */
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include <round.h>
#include <stdint.h>
#include "threads/init.h"
#include "userprog/process.h"
#include "threads/palloc.h"
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* User Memory 접근 보호 함수 선언*/
void check_user_address(const void *uaddr);
void check_user_buffer(char *buffer, size_t size);

/* System call을 위한 함수 선언 */
void sys_exit(int status);
int sys_write(int fd, const void *buffer, unsigned size);
int sys_read(int fd, void *buffer, unsigned size);
void sys_halt(void);
int sys_dup2(int oldfd, int newfd);

/* fd 할당/해제를 위한 함수 선언 */
static int allocate_fd(struct file *f);
static void free_fd(int fd);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface
	시스템 콜을 받아줄 switch 문 추가
*/
void syscall_handler(struct intr_frame *f UNUSED)
{
	// TODO: Your implementation goes here.
	// printf("system call!\n");
	// thread_exit();

	int syscall_num = (int)f->R.rax;

	switch (syscall_num)
	{
	/* void exit(int status); 호출 시 */
	case SYS_EXIT:
	{
		int status = (int)f->R.rdi;

		/* status 가 유효한 포인터가 아니고 단순 값이므로 검사 불필요 */
		sys_exit(status);
		break;
	}
	/* int write(int fd, const void *buffer, unsigned size); 호출 시 */
	case SYS_WRITE:
	{
		int fd = (int)f->R.rdi;			 // 단순 값
		void *buffer = (void *)f->R.rsi; // 검사가 필요
		size_t size = (size_t)f->R.rdx;	 // 단순 값

		if (size == 0)
		{
			f->R.rax = 0;
			break;
		}
		/* buffer 포인터가 유효한 유저 영역인지 */
		check_user_address(buffer);

		/* buffer 가 cnt 바이트 연속으로 유효한지 (페이지 경계를 넘어가도) */
		check_user_buffer((char *)buffer, size);

		/* 이제 안전하므로 출력 */
		f->R.rax = sys_write(fd, buffer, size);
		break;
	}
	/* int wait(tid_t tid); 호출 시	*/
	case SYS_WAIT:
	{
		tid_t child_tid = (tid_t)f->R.rdi;
		f->R.rax = process_wait(child_tid);
		break;
	}
	/* void halt(void); 호출 시*/
	case SYS_HALT:
	{
		sys_halt();
		break;
	}
	/* int exec(const char *file); 호출 시 */
	case SYS_EXEC:
	{
		char *file_name = (char *)f->R.rdi;
		check_user_address(file_name);
		check_user_buffer(file_name, strlen(file_name) + 1);
		char *kpage = palloc_get_page(PAL_USER | PAL_ZERO);
		if (!kpage)
			sys_exit(-1);
		// f->R.rax = -1;
		else
		{
			strlcpy(kpage, file_name, PGSIZE);
			// f->R.rax = process_exec(kpage);
			/* process_exec이 성공하면 여기로 돌아오지 않음.
			   실패 시 -1을 반환하므로, 즉시 종료. */
			int exec_ret = process_exec(kpage);
			sys_exit(exec_ret); // exec_ret == -1 이므로 exit(-1)
		}
		break;
	}
	/* pid_t fork (const char *thread_name); 호출 시 */
	case SYS_FORK:
	{
		char *thread_name = (char *)f->R.rdi;
		check_user_buffer(thread_name, strlen(thread_name) + 1);
		f->R.rax = process_fork(thread_name, f);
		break;
	}
	/* int read (int fd, void *buffer, unsigned size); 호출 시 */
	case SYS_READ:
	{
		int fd = (int)f->R.rdi;
		void *buffer = (void *)f->R.rsi;
		unsigned size = (unsigned)f->R.rdx;
		if (size == 0)
		{
			f->R.rax = 0;
			break;
		}
		check_user_address(buffer);
		check_user_buffer(buffer, size);

		f->R.rax = sys_read(fd, buffer, size);

		break;
	}
	/* bool create (const char *file, unsigned initial_size); 호출 시 */
	case SYS_CREATE:
	{
		const char *file = (const char *)f->R.rdi;
		unsigned initial_size = (unsigned)f->R.rsi;
		check_user_address(file);
		check_user_buffer((char *)file, strlen(file) + 1);

		f->R.rax = filesys_create(file, initial_size);

		break;
	}
	/* bool remove (const char *file); 호출 시 */
	case SYS_REMOVE:
	{
		const char *file = (const char *)f->R.rdi;
		check_user_address(file);
		check_user_buffer((char *)file, strlen(file) + 1);

		f->R.rax = filesys_remove(file);

		break;
	}
	/* int open (const char *file) 호출 시*/
	case SYS_OPEN:
	{
		const char *file = (const char *)f->R.rdi;
		check_user_address(file);
		check_user_buffer(file, strlen(file) + 1);

		struct file *fptr = filesys_open(file);

		if (!fptr)
		{
			f->R.rax = -1;
		}
		else
		{
			int fd = allocate_fd(fptr);
			if (fd < 0)
				file_close(fptr);
			f->R.rax = fd;
		}
		break;
	}
	/* void close (int fd); 호출 시*/
	case SYS_CLOSE:
	{
		int fd = (int)f->R.rdi;
		if (fd > 1 && fd < MAX_FD && thread_current()->fd_table[fd])
		{
			file_close(thread_current()->fd_table[fd]);

			free_fd(fd);
			f->R.rax = 0;
		}
		else
			f->R.rax = -1;
		break;
	}
	/* int filesize (int fd); 호출 시 */
	case SYS_FILESIZE:
	{
		int fd = (int)f->R.rdi;
		struct file *fptr = NULL;
		if (fd > 1 && fd < MAX_FD)
			fptr = thread_current()->fd_table[fd];

		f->R.rax = fptr ? (off_t)file_length(fptr) : -1;

		break;
	}
	/* void seek (int fd, unsigned position); 호출 시 */
	case SYS_SEEK:
	{
		int fd = (int)f->R.rdi;
		unsigned pos = (unsigned)f->R.rsi;
		struct file *fptr = NULL;
		if (fd > 1 && fd < MAX_FD)
			fptr = thread_current()->fd_table[fd];
		if (fptr)
		{
			file_seek(fptr, pos);
		}
		f->R.rax = 0;
		break;
	}
	/* unsigned tell (int fd); 호출 시 */
	case SYS_TELL:
	{
		int fd = (int)f->R.rdi;
		struct file *fptr = NULL;
		if (fd > 1 && fd < MAX_FD)
			fptr = thread_current()->fd_table[fd];

		f->R.rax = fptr ? (unsigned)file_tell(fptr) : -1;

		break;
	}
	/* int dup2(int oldfd, int newfd); 호출 시 */
	case SYS_DUP2:
	{
		int oldfd = (int)f->R.rdi;
		int newfd = (int)f->R.rsi;
		f->R.rax = sys_dup2(oldfd, newfd);
		break;
	}
	default:
		sys_exit(-1);
	}
}

/*  check_user_address(const void *uaddr){}
	널 포인터 차단 : !uaddr → NULL 이면 즉시 프로세스 종료
	유저 영역 검사 : !is_user_vaddr(uaddr) → 주소가 `PHYS_BASE` 이상(커널 영역)에 있으면 종료
	매핑 여부 검사 : pml4_get_page(..., uaddr) == NULL => 가상 → 물리 매핑이 안 돼 있으면 종료
*/
void check_user_address(const void *uaddr)
{
	if (!uaddr || !is_user_vaddr(uaddr) || pml4_get_page(thread_current()->pml4, uaddr) == NULL)
	{
		sys_exit(-1);
	}
}

/*	check_user_buffer(char *buffer, size_t size){}
	버퍼가 가리키는 메모리 영역 전체가 유저 전용 영역(커널 영역 외)이고
										실제 물리 메모리와 매핑되어 있는지 검사
	버퍼가 N개 페이지에 걸쳐 있어도 각 페이지의 첫 유효주소 한바이트만 검사해도 안전
*/
void check_user_buffer(char *buffer, size_t size)
{
	const uint8_t *ptr = (uint8_t *)buffer; // 1바이트 단위 포인터로 변환
	size_t ofs = 0;							// 현재 검사위치를 나타내는 오프셋 변수

	if (size == 0)
		return;

	while (ofs < size) // 사이즈만큼 다 검사
	{
		void *addr = (void *)(ptr + ofs); // 검사할 현재 주소 계산
		check_user_address(addr);		  // 한 바이트라도 user_address 검사

		/* PGSIZE : 페이지 크기(4KB)
			pg_ofs() : 주소가 페이지 내부에서 얼마나 떨어져 있는지
		*/
		size_t left = PGSIZE - pg_ofs(addr); // 이 주소가 속한 페이지의 남은 바이트 수 계산
		ofs += left;						 // 한번 검사한 영역 만큼 오프셋 건너뛰기
	}
}

/* thread 종료를 위한 sys_exit()*/
void sys_exit(int status)
{
	struct thread *curr = thread_current();
	if (curr->exec_prog != NULL)
	{
		file_allow_write(curr->exec_prog);
		file_close(curr->exec_prog);
	}
	curr->exit_status = status;
	thread_exit(); // curr->status -> THREAD_DYING
}

int sys_write(int fd, const void *buffer, unsigned size)
{
	int ret;

	// 1) 파일 디스크립터 테이블에 매핑된 파일이 있으면,
	//    무조건 그 파일로 쓰기 (dup2로 덮어쓴 stdout 포함)
	// 표준 출력 처리는 file_write에서 했음
	if (fd >= 0 && fd < MAX_FD && thread_current()->fd_table[fd])
	{
		ret = file_write(thread_current()->fd_table[fd], buffer, size);
	}
	else
	{
		ret = -1;
	}
	return ret;
}

/* halt() power_off를 위한 sys_halt()*/
void sys_halt(void)
{
	power_off();
}

int sys_read(int fd, void *buffer, unsigned size)
{
	int ret;
	// 1) 파일 디스크립터 테이블에 매핑된 파일이 있으면,
	//    무조건 그 파일로 쓰기 (dup2로 덮어쓴 stdin 포함)
	// 표준 입력 처리는 file_read에서 했음
	if (fd >= 0 && fd < MAX_FD && thread_current()->fd_table[fd])
	{
		ret = file_read(thread_current()->fd_table[fd], buffer, size);
	}
	else
	{
		ret = -1;
	}
	return ret;
}

/* dup2를 위한 sys_dup2 */
int sys_dup2(int oldfd, int newfd)
{
	struct thread *cur = thread_current();

	// 유효성 검사
	if (oldfd < 0 || oldfd >= MAX_FD || newfd < 0 || newfd >= MAX_FD)
		return -1;

	// oldfd가 열려 있지 않으면 실패(EBADF)
	if (cur->fd_table[oldfd] == NULL)
		return -1;

	// fd 같으면 그냥 바로 리턴
	if (oldfd == newfd)
		return newfd;

	// newfd 열려있다면 닫아주고
	if (cur->fd_table[newfd] != NULL) // newfd가 열려있다면
		file_close(cur->fd_table[newfd]);

	cur->fd_table[newfd] = file_dup2(cur->fd_table[oldfd]);

	return newfd;
}

/* fd 할당 / 해제 헬퍼 함수*/
static int allocate_fd(struct file *f)
{
	struct thread *cur = thread_current();
	for (int fd = cur->next_fd; fd < MAX_FD; fd++)
	{
		if (cur->fd_table[fd] == NULL)
		{
			cur->fd_table[fd] = f;
			cur->next_fd = fd + 1;
			return fd;
		}
	}
	return -1;
}

static void free_fd(int fd)
{
	struct thread *cur = thread_current();
	cur->fd_table[fd] = NULL;
	if (fd < cur->next_fd)
		cur->next_fd = fd;
}