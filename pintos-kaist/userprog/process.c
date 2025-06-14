#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* process_wait를 위한 sema*/
#include "threads/synch.h"

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);

/* General process initializer for initd and other process. */
static void
process_init(void)
{
	struct thread *cur = thread_current();

	cur->fd_table = palloc_get_page(PAL_ZERO);
	if (cur->fd_table == NULL)
		PANIC("process_init: cannot alloc fd_table");

	/* console_in/out 은 이미 thread_init() 시점에 초기화해 두었다고 가정 */
	cur->fd_table[0] = &console_in;
	cur->fd_table[1] = &console_out;
	cur->next_fd = 2;
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name)
{
	char *fn_copy;
	tid_t tid;
	struct thread *parent = thread_current();

	/* 1) Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE);

	/* 프로그램 이름, 인자 추출 첫번째 공백으로 자름*/
	char prog_name[NAME_MAX + 1];
	size_t namelen = strcspn(fn_copy, " ");
	if (namelen > NAME_MAX)
		namelen = NAME_MAX;
	strlcpy(prog_name, fn_copy, namelen + 1);

	/* 2) child_status 만들고 부모 리스트에 등록 */
	struct child_status *c = malloc(sizeof *c);
	if (!c)
	{
		palloc_free_page(fn_copy);
		return TID_ERROR;
	}
	sema_init(&c->sema, 0);
	c->has_exited = false;
	c->exit_status = -1;
	list_push_back(&parent->children, &c->elem);

	/* 3) Create a new thread to execute FILE_NAME. */
	tid = thread_create(prog_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
	{
		list_remove(&c->elem);
		free(c);
		palloc_free_page(fn_copy);
		return TID_ERROR;
	}
	/* 4) 성공하면 child_status에 TID 저장 */
	c->tid = tid;
	thread_by_tid(tid)->parent_tid = parent->tid;

	/* 5) 부모는 initd 준비까지 기다렸다가 리턴 */
	sema_down(&c->sema);
	list_remove(&c->elem);
	free(c);

	return tid;
}

/* A thread function that launches first user process. */
static void
initd(void *f_name)
{
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif

	process_init();

	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_ UNUSED)
{
	struct thread *parent = thread_current();
	/* 1) 부모의 intr_frame 을 페이지 단위로 복사 */
	struct intr_frame *child_if = palloc_get_page(PAL_ZERO);
	if (!child_if)
		return TID_ERROR;
	*child_if = *if_;
	/* 자식 프로세스가 fork 리턴 받을 땐 0이 되어야 함*/
	child_if->R.rax = 0;

	/* 2) 부모의 children 리스트에 등록할 구조체 할당 */
	struct child_status *c = malloc(sizeof *c);
	if (!c)
	{
		palloc_free_page(child_if);
		return TID_ERROR;
	}
	sema_init(&c->sema, 0);
	c->has_exited = false;
	c->exit_status = -1;
	list_push_back(&parent->children, &c->elem);

	/* 3) 실제 자식 스레드 생성 (__do_fork 가 실행될 것) */
	tid_t child_tid = thread_create(name, PRI_DEFAULT, __do_fork, child_if);
	if (child_tid == TID_ERROR)
	{
		list_remove(&c->elem);
		free(c);
		palloc_free_page(child_if);
		return TID_ERROR;
	}
	/* 4) 자식 TID를 child_status 에 저장 후 부모에 리턴 */
	c->tid = child_tid;

	struct thread *child = thread_by_tid(child_tid);
	child->parent_tid = parent->tid;
	sema_down(&c->sema);
	return child_tid;
	/* Clone current thread to new thread.*/
	// return thread_create(name,
	// 					 PRI_DEFAULT, __do_fork, thread_current());
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (!is_user_vaddr(va))
		return true;
	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page(parent->pml4, va);
	if (parent_page == NULL)
		return true;
	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL)
		return false;
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = (*pte & PTE_W) != 0;
	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork(void *aux)
{
	struct intr_frame child_if = *(struct intr_frame *)aux;
	palloc_free_page(aux);
	struct thread *current = thread_current();
	struct thread *parent = thread_by_tid(current->parent_tid);
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	bool succ = true;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate(current);
#ifdef VM
	supplemental_page_table_init(&current->spt);
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	for (int fd = 0; fd < MAX_FD; fd++)
	{
		struct file *f = parent->fd_table[fd];
		if (f == NULL)
		{
			current->fd_table[fd] = NULL;
		}
		else if (f == &console_in || f == &console_out)
		{
			/* stdin : console_in 공유 */
			current->fd_table[fd] = f;
		}
		else
			current->fd_table[fd] = file_duplicate(f);
	}
	current->next_fd = parent->next_fd;

	if (parent != NULL)
	{
		struct list_elem *e;
		for (e = list_begin(&parent->children);
			 e != list_end(&parent->children);
			 e = list_next(e))
		{
			struct child_status *c = list_entry(e, struct child_status, elem);
			if (c->tid == current->tid)
			{
				c->has_exited = false; // 아직 exit 전
				sema_up(&c->sema);
				break;
			}
		}
	}
	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret(&child_if);
error:
	thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup();
	// #ifdef VM
	// 	/* project 3) 새 프로그램 로드를 위해 빈 SPT로 다시 초기화 추가*/
	// 	supplemental_page_table_init(&thread_current()->spt);
	// #endif

	/* And then load the binary */
	success = load(file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page(file_name);
	if (!success)
	{
		process_cleanup(); // 비정상 종료시 자원 해제
		return -1;
	}
	/* Start switched process. */
	do_iret(&_if);
	NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid UNUSED)
{
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	struct thread *cur = thread_current();
	struct list_elem *e;
	struct child_status *c = NULL;

	/* 1) 자식 리스트에서 기다릴 child_tid 찾기 */
	for (e = list_begin(&cur->children); e != list_end(&cur->children); e = list_next(e))
	{
		struct child_status *tmp = list_entry(e, struct child_status, elem);
		if (tmp->tid == child_tid)
		{
			c = tmp;
			break;
		}
	}
	/* 내 자식이 아니거나 이미 wait 한 경우 */
	if (c == NULL)
		return -1;

	/* 2) 아직 자식이 exit() 안 했으면 대기 */
	if (!c->has_exited)
		sema_down(&c->sema);

	/* 3) 자식 exit_status 가져온 뒤 정리 */
	int status = c->exit_status;
	list_remove(&c->elem);
	free(c);
	return status;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	struct thread *curr = thread_current();
	struct thread *parent;
	struct list_elem *e;
	struct child_status *c;

	/* --- USERPROG 에서만 종료메시지 찍게 설정하기 --- */
	if (curr->parent_tid != TID_ERROR)
	{
		/* 1) 종료 메시지 출력 */
		printf("%s: exit(%d)\n", curr->name, curr->exit_status);

#ifdef VM
		/* 커널 스레드가 아닌, 사용자 프로세스 페이지만 */
		if (curr->pml4 != NULL) /* 또는 SPT가 초기화된 스레드만 */
		{
			/* 1) SPT 해시 전체 순회하며 file-backed 페이지만 골라 do_munmap 호출 */
			void **starts = NULL;
			size_t nstarts = 0;

			struct hash_iterator hi;
			hash_first(&hi, &curr->spt.spt_hash);
			while (hash_next(&hi))
			{
				struct page *p = hash_entry(hash_cur(&hi), struct page, hash_elem);

				/* 2) 파일 매핑된 페이지만 처리 (VM_FILE 타입) */
				if (p->operations->type == VM_FILE)
				{
					struct file_page *aux = p->uninit.aux;

					/* 3) 매핑의 첫 페이지(start_addr)에서만 수집 */
					if (p->va == aux->start_addr)
					{
						/* 4) 중복 저장 방지: 이미 수집된 주소인지 확인 */
						bool seen = false;
						for (size_t i = 0; i < nstarts; i++)
							if (starts[i] == aux->start_addr)
							{
								seen = true;
								break;
							}

						/* 5) 새로 발견된 시작 주소이면 배열에 추가 */
						if (!seen)
						{
							void **tmp = realloc(starts, sizeof(void *) * (nstarts + 1));
							if (!tmp)
								break;
							starts = tmp;
							starts[nstarts++] = aux->start_addr;
						}
					}
				}
			}
			/* 2) Now unmap each mapping (this mutates the hash, but it’s safe because
		   we’re no longer iterating it) */
			for (size_t i = 0; i < nstarts; i++)
				do_munmap(starts[i]);

			free(starts);
		}
#endif
		/* 2) 부모에게 exit 상태 전달 및 sema_up() */
		if (curr->parent_tid != TID_ERROR)
		{
			parent = thread_by_tid(curr->parent_tid);
			if (parent != NULL)
			{
				for (e = list_begin(&parent->children);
					 e != list_end(&parent->children);
					 e = list_next(e))
				{
					c = list_entry(e, struct child_status, elem);
					if (c->tid == curr->tid)
					{
						c->exit_status = curr->exit_status;
						c->has_exited = true;
						sema_up(&c->sema);
						break;
					}
				}
			}
		}

		// TODO: fd_table 순회하여 file_close()
		for (int fd = 0; fd < MAX_FD; fd++)
		{
			struct file *f = curr->fd_table[fd];
			if (f != NULL && f != &console_in /* stdin 예외 */
				&& f != &console_out)		  /* stdout 예외 */
			{
				file_close(curr->fd_table[fd]);
				curr->fd_table[fd] = NULL;
			}
		}
	}
	process_cleanup();
}

/* Free the current process's resources. */
static void
process_cleanup(void)
{
	struct thread *curr = thread_current();

#ifdef VM
	supplemental_page_table_kill(&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL)
	{
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate(NULL);
		pml4_destroy(pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
	/* Activate thread's page tables. */
	pml4_activate(next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0			/* Ignore. */
#define PT_LOAD 1			/* Loadable segment. */
#define PT_DYNAMIC 2		/* Dynamic linking info. */
#define PT_INTERP 3			/* Name of dynamic loader. */
#define PT_NOTE 4			/* Auxiliary info. */
#define PT_SHLIB 5			/* Reserved. */
#define PT_PHDR 6			/* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load(const char *file_name, struct intr_frame *if_)
{
	struct thread *t = thread_current();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	/* argc, argv 받는 부분 추가 argument passing
		1) file_name 문자열을 strtok_r로 토큰화
		saveptr : strtok_r 내부가 다음 검색 위치를 기억하기 위해 사용하는 포인터
		token : 잘라낸 각 토큰(단어)을 가리키는 포인터
		argc : 토큰 개수를 세는 카운터
		argv[64] : 잘라낸 토큰들의 시작 주소를 순서대로 저장할 배열
	*/
	char *saveptr, *token;
	int argc = 0;
	char *argv[64];
	for (token = strtok_r(file_name, " ", &saveptr); // 첫 토큰 얻기
		 token != NULL;								 // token이 NULL될때까지 반복
		 token = strtok_r(NULL, " ", &saveptr))		 // 다음 토큰 얻기
	{
		if (argc >= 63)
			break; // 63개 까지만
		argv[argc++] = token;
	}
	argv[argc] = NULL; // 마지막은 null

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create();
	if (t->pml4 == NULL)
		goto done;
	process_activate(thread_current());
	/* Open executable file.
		filename 전체가 아닌 첫번째 인자(프로그램 이름)
	*/
	file = filesys_open(argv[0]);
	if (file == NULL)
	{
		printf("load: %s: open failed\n", argv[0]);
		goto done;
	}

	/* rox를 위한 deny 추가*/
	file_deny_write(file);
	t->exec_prog = file;

	/* Read and verify executable header. */
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
		|| ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
	{
		printf("load: %s: error loading executable\n", argv[0]);
		goto done;
	}
	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++)
	{
		struct Phdr phdr;
		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs);
		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type)
		{
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file))
			{
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0)
				{
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					read_bytes = page_offset + phdr.p_filesz;
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
				}
				else
				{
					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment(file, file_page, (void *)mem_page,
								  read_bytes, zero_bytes, writable))
					goto done;
			}
			else
				goto done;
			break;
		}
	}
	/* Set up stack. */
	if (!setup_stack(if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* 2) _if.rsp(=initial esp) 기준으로 문자열 복사
		-> 워드 정렬 -> argv 배열 푸시 -> 가짜리턴주소 푸시 -> 레지스터 설정
		if_->rsp 유저 스택 최상단
	*/
	/* 문자열 복사 */
	for (int i = argc - 1; i >= 0; i--)
	{
		size_t len = strlen(argv[i]) + 1; // NULL('\0')까지 포함한 문자열 길이
		if_->rsp -= len;				  // esp를 문자열 크기만큼 낮춰, 복사할 공간 확보
		memcpy(if_->rsp, argv[i], len);	  // 스택(esp 위치)에 실제 문자열 복사
		argv[i] = if_->rsp;				  // argv[i]에 복사된 문자열의 스택 주소 저장
	}

	/* 워드 정렬 (8바이트 경계) */
	if_->rsp = (char *)((uintptr_t)(if_->rsp) & ~(uintptr_t)0x7);

	/* argv 배열, NULL 종단자 푸시 */
	if_->rsp -= sizeof(char *); // 포인터는 8바이트!
	*(char **)if_->rsp = NULL;
	for (int i = argc - 1; i >= 0; i--)
	{
		if_->rsp -= sizeof(char *);
		*(char **)if_->rsp = argv[i];
	}

	/* 가짜 리턴 주소 푸시 */
	if_->rsp -= sizeof(void *);
	*(void **)if_->rsp = NULL;

	/* 레지스터 및 스택 포인터 업데이트 */
	if_->R.rdi = argc;
	if_->R.rsi = (uintptr_t)(if_->rsp + sizeof(void *));

	success = true;

done:
	// load 이후에 file_close 해버리면 안되고, exit()가 되었을 때 파일을 닫아야 함
	/* We arrive here whether the load is successful or not. */
	// if (file != NULL)
	// {
	// 	file_close(file);
	// }
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
{
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t)file_length(file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	file_seek(file, ofs);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page(PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
		{
			palloc_free_page(kpage);
			return false;
		}
		memset(kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page(upage, kpage, writable))
		{
			printf("fail\n");
			palloc_free_page(kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack(struct intr_frame *if_)
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (kpage != NULL)
	{
		success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page(kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment(struct page *page, void *aux)
{
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct load_info *info = (struct load_info *)aux;
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

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		/* 로드를 위해 필요한 정보를 가진 load_info 구조체 만들었음*/
		struct load_info *aux = (struct load_info *)malloc(sizeof(*aux));
		if (aux == NULL)
			return false;

		aux->file = file;
		aux->offset = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->writable = writable;

		if (!vm_alloc_page_with_initializer(VM_ANON, upage,
											writable, lazy_load_segment, aux))
		{
			free(aux);
			return false;
		}

		/* Advance. */
		/* 4) 다음 반복을 위해 각 값 갱신 */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		/* 파일에서 연속적으로 다음 페이지를 읽어 오기 위함 */
		ofs += page_read_bytes; /* ofs를 읽어들인 만큼만 증가 */
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack(struct intr_frame *if_)
{
	bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	/* ANON 스택 페이지 하나 할당 받음
		writable : true
	*/
	if (!vm_alloc_page(VM_ANON, stack_bottom, true))
		return false;

	// 할당 받은 페이지에 바로 물리 프레임을 매핑한다.
	if (!vm_claim_page(stack_bottom))
		return false;

	// rsp를 변경한다. (argument_stack에서 이 위치부터 인자를 push한다.)
	if_->rsp = USER_STACK;

	// 스레드가 지금까지 할당받은 스택의 최하단(가장 낮은 주소)를 나타내는 경계
	thread_current()->stack_bottom = stack_bottom;

	success = true;

	return success;
}
#endif /* VM */
