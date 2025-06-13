#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

/* 익명 페이지가 스왑 디스크의 어느 슬롯에 저장되었는지를 기록하는 용도*/
struct anon_page
{
    int swap_slot; //  스왑 디스크의 슬롯 인덱스 - 페이지가 아직 스왑되지 않았으면 -1
};

void vm_anon_init(void);
bool anon_initializer(struct page *page, enum vm_type type, void *kva);

#endif
