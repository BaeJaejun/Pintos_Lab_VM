#include "filesys/file.h"
#include <debug.h>
#include "filesys/inode.h"
#include "threads/malloc.h"

#include "threads/synch.h" /* 락을 위한 include*/
#include "filesys/filesys.h"
/* An open file. */
struct file
{
	struct inode *inode; /* File's inode. */
	off_t pos;			 /* Current position. */
	bool deny_write;	 /* Has file_deny_write() been called? */
	int ref_cnt;		 /* fd 참조 개수 */
};

/* 콘솔을 가리키는 가짜 file 구조체 두 개 */
struct file console_in;
struct file console_out;

void console_file_init(void)
{
	/* inode == NULL 이면 콘솔 입출력 분기로 처리하도록 */
	console_in.inode = NULL;
	console_in.pos = 0;
	console_in.deny_write = false;
	console_in.ref_cnt = 1;

	console_out.inode = NULL;
	console_out.pos = 0;
	console_out.deny_write = false;
	console_out.ref_cnt = 1;
}

/* Opens a file for the given INODE, of which it takes ownership,
 * and returns the new file.  Returns a null pointer if an
 * allocation fails or if INODE is null. */
struct file *
file_open(struct inode *inode)
{
	lock_acquire(&filesys_lock);
	struct file *file = calloc(1, sizeof *file);
	if (inode != NULL && file != NULL)
	{
		file->inode = inode;
		file->pos = 0;
		file->deny_write = false;
		file->ref_cnt = 1; // 초기 참조 카운트
	}
	else
	{
		inode_close(inode);
		free(file);
		file = NULL;
	}
	lock_release(&filesys_lock);
	return file;
}

/* Opens and returns a new file for the same inode as FILE.
 * Returns a null pointer if unsuccessful. */
struct file *
file_reopen(struct file *file)
{
	return file_open(inode_reopen(file->inode));
}

/* Duplicate the file object including attributes and returns a new file for the
 * same inode as FILE. Returns a null pointer if unsuccessful. */
struct file *
file_duplicate(struct file *file)
{
	struct file *nfile = file_open(inode_reopen(file->inode));
	if (nfile)
	{
		nfile->pos = file->pos;
		if (file->deny_write)
			file_deny_write(nfile);
	}
	return nfile;
}

/* dup2를 위한 함수 선언*/
struct file *
file_dup2(struct file *file)
{
	lock_acquire(&filesys_lock);
	file->ref_cnt++;
	lock_release(&filesys_lock);

	return file;
}

/* Closes FILE. */
void file_close(struct file *file)
{
	if (file == NULL)
		return;

	if (file == &console_in || file == &console_out)
		return;

	lock_acquire(&filesys_lock);

	/* 먼저 참조 카운트만 감소 */
	file->ref_cnt--;

	/* 마지막 복제본이 닫힐 때만 실제로 해제 작업 수행 */
	if (file->ref_cnt == 0)
	{
		file_allow_write(file);
		inode_close(file->inode);
		free(file);
	}

	lock_release(&filesys_lock);
}

/* Returns the inode encapsulated by FILE. */
struct inode *
file_get_inode(struct file *file)
{
	return file->inode;
}

/* Reads SIZE bytes from FILE into BUFFER,
 * starting at the file's current position.
 * Returns the number of bytes actually read,
 * which may be less than SIZE if end of file is reached.
 * Advances FILE's position by the number of bytes read. */
off_t file_read(struct file *file, void *buffer, off_t size)
{
	lock_acquire(&filesys_lock);

	/* 출력디스크립터 일때 */
	if (file == &console_out)
	{
		lock_release(&filesys_lock);
		return -1;
	}
	/* 표준 입력 일때 */
	else if (file->inode == NULL)
	{
		/* stdin 역할 */
		for (off_t i = 0; i < size; i++)
			((char *)buffer)[i] = input_getc();
		lock_release(&filesys_lock);

		return size;
	}

	off_t bytes_read = inode_read_at(file->inode, buffer, size, file->pos);
	file->pos += bytes_read;
	lock_release(&filesys_lock);

	return bytes_read;
}

/* Reads SIZE bytes from FILE into BUFFER,
 * starting at offset FILE_OFS in the file.
 * Returns the number of bytes actually read,
 * which may be less than SIZE if end of file is reached.
 * The file's current position is unaffected. */
off_t file_read_at(struct file *file, void *buffer, off_t size, off_t file_ofs)
{
	return inode_read_at(file->inode, buffer, size, file_ofs);
}

/* Writes SIZE bytes from BUFFER into FILE,
 * starting at the file's current position.
 * Returns the number of bytes actually written,
 * which may be less than SIZE if end of file is reached.
 * (Normally we'd grow the file in that case, but file growth is
 * not yet implemented.)
 * Advances FILE's position by the number of bytes read. */
off_t file_write(struct file *file, const void *buffer, off_t size)
{
	lock_acquire(&filesys_lock);

	/* 입력디스크립터 일때 */
	if (file == &console_in)
	{
		lock_release(&filesys_lock);
		return -1;
	}
	/* 표준 출력력 일때 */
	else if (file->inode == NULL)
	{
		/* stdout 역할 */
		putbuf(buffer, size);
		lock_release(&filesys_lock);

		return size;
	}

	off_t bytes_written = inode_write_at(file->inode, buffer, size, file->pos);
	file->pos += bytes_written;
	lock_release(&filesys_lock);
	return bytes_written;
}

/* Writes SIZE bytes from BUFFER into FILE,
 * starting at offset FILE_OFS in the file.
 * Returns the number of bytes actually written,
 * which may be less than SIZE if end of file is reached.
 * (Normally we'd grow the file in that case, but file growth is
 * not yet implemented.)
 * The file's current position is unaffected. */
off_t file_write_at(struct file *file, const void *buffer, off_t size,
					off_t file_ofs)
{
	return inode_write_at(file->inode, buffer, size, file_ofs);
}

/* Prevents write operations on FILE's underlying inode
 * until file_allow_write() is called or FILE is closed. */
void file_deny_write(struct file *file)
{
	ASSERT(file != NULL);
	if (!file->deny_write)
	{
		file->deny_write = true;
		inode_deny_write(file->inode);
	}
}

/* Re-enables write operations on FILE's underlying inode.
 * (Writes might still be denied by some other file that has the
 * same inode open.) */
void file_allow_write(struct file *file)
{
	ASSERT(file != NULL);
	if (file->deny_write)
	{
		file->deny_write = false;
		inode_allow_write(file->inode);
	}
}

/* Returns the size of FILE in bytes. */
off_t file_length(struct file *file)
{
	ASSERT(file != NULL);
	return inode_length(file->inode);
}

/* Sets the current position in FILE to NEW_POS bytes from the
 * start of the file. */
void file_seek(struct file *file, off_t new_pos)
{
	ASSERT(file != NULL);
	ASSERT(new_pos >= 0);
	file->pos = new_pos;
}

/* Returns the current position in FILE as a byte offset from the
 * start of the file. */
off_t file_tell(struct file *file)
{
	ASSERT(file != NULL);
	return file->pos;
}
