#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd(const char *file_name);
tid_t process_fork(const char *name, struct intr_frame *if_);
int process_exec(void *f_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(struct thread *next);

/* lazy load를 위한 구조체, 페이지 불러올 때 필요한 정보들 */
struct load_info
{
    struct file *file;   /* 원본 파일 핸들 */
    off_t offset;        /* 파일 읽기 시작 오프셋 */
    uint32_t read_bytes; /* 파일에서 읽어야 할 바이트 수 */
    uint32_t zero_bytes; /* 남은 부분을 0으로 채울 바이트 수 */
    bool writable;       /* 페이지를 읽기/쓰기 모드로 매핑할지 여부 */
};
#endif /* userprog/process.h */
