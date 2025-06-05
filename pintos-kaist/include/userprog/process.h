#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd(const char *file_name);
tid_t process_fork(const char *name, struct intr_frame *if_);
int process_exec(void *f_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(struct thread *next);

struct lazy_load_aux
{
    struct file *file;   /* 파일 핸들 */
    off_t offset;        /* 파일 내에서 읽기를 시작할 오프셋 */
    uint32_t read_bytes; /* 이 페이지에 대해 파일에서 읽어들일 실제 바이트 수 */
    uint32_t zero_bytes; /* 그 나머지를 0으로 채울 바이트 수 */
    bool writable;       /* 페이지를 읽기/쓰기 모드로 매핑할지 여부 */
};
#endif /* userprog/process.h */
