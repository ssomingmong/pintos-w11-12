#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/init.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "lib/kernel/stdio.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"


void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static int fd_add_file (struct file *file);
static struct file *fd_get_file (int fd);
static void fd_close_file (int fd);

struct lock filesys_lock;


static void exit_with_status(int status){
	thread_current()->exit_status = status;
	thread_exit();
}


static void check_address(const void *addr) {
	if(addr == NULL || is_user_vaddr(addr) == 0 || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		exit_with_status(-1);
	}
}


static void check_buffer(const void *buffer, unsigned size) {
	const char *cur = buffer;
	unsigned i;


	if (size == 0)
		return;

	for(i = 0; i < size; i++) {
		check_address(cur + i);
	}
}


static void check_string(const char *str) {
	if(str == NULL)
		exit_with_status(-1);
	while(1) {
		check_address(str);
		if(*str == '\0')
			break;
		str++;
	}
}



static int
fd_add_file (struct file *file) {
	struct thread *cur = thread_current ();
	int i;

	if (file == NULL)
		return -1;

	for (i = 2; i < MAX_FD; i++) {
		int fd = cur->next_fd;

		if (fd < 2 || fd >= MAX_FD)
			fd = 2;

		if (cur->fd_table[fd] == NULL) {
			cur->fd_table[fd] = file;
			cur->next_fd = fd + 1;
			return fd;
		}
		cur->next_fd = fd + 1;
	}
	return -1;
}


static struct file *
fd_get_file (int fd) {
	struct thread *cur = thread_current ();

	if (fd < 2 || fd >= MAX_FD)
		return NULL;
	return cur->fd_table[fd];
}


static void
fd_close_file (int fd) {
	struct thread *cur = thread_current ();
	struct file *file;

	if (fd < 2 || fd >= MAX_FD)
		return;

	file = cur->fd_table[fd];
	if (file == NULL)
		return;

	cur->fd_table[fd] = NULL;
	lock_acquire(&filesys_lock);
	file_close(file);
	lock_release(&filesys_lock);
}
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {


	switch(f->R.rax){
		case SYS_HALT:


			power_off();
			break;

		case SYS_EXIT:

			exit_with_status(f->R.rdi);
			break;

		case SYS_WRITE: {


			int fd = (int) f->R.rdi;
			const void *buffer = (const void *) f->R.rsi;
			unsigned size = (unsigned) f->R.rdx;
			struct file *file;


			if (size > 0) {
				int is_valid = 1;

				if (buffer == NULL) {
					is_valid = 0;
				} else {
					char *start_page = pg_round_down((char *) buffer);
					char *end_page = pg_round_down((char *) buffer + size - 1);

					for (char *page = start_page; page <= end_page; page += PGSIZE) {
						if (!is_user_vaddr(page) ||
								pml4_get_page(thread_current()->pml4, page) == NULL) {
							is_valid = 0;
							break;
						}
					}
				}

				if (!is_valid)
					exit_with_status(-1);
			}

			if(fd == 1) {


				putbuf(buffer, size);



				f->R.rax = size;
			}
			else if (fd == 0) {

				f->R.rax = -1;
			}
			else {

				file = fd_get_file(fd);
				if (file == NULL) {
					f->R.rax = -1;
				} else {
					lock_acquire(&filesys_lock);
					f->R.rax = file_write(file, buffer, size);
					lock_release(&filesys_lock);
				}
			}
			break;
		}

		case SYS_CREATE: {

			char *filename = (char *) f->R.rdi;

			unsigned size = (unsigned) f->R.rsi;


			// check_address(filename);
			check_string(filename);

			lock_acquire(&filesys_lock);

			f->R.rax = filesys_create(filename, size);

			lock_release(&filesys_lock);

			break;
		}

		case SYS_REMOVE: {

			char *filename = (char *) f->R.rdi;

			check_string(filename);
			lock_acquire(&filesys_lock);
			f->R.rax = filesys_remove(filename);
			lock_release(&filesys_lock);
			break;
		}

		case SYS_OPEN: {

			char *filename = (char *) f->R.rdi;
			struct file *file;
			int fd = -1;

			check_string(filename);
			lock_acquire(&filesys_lock);
			file = filesys_open(filename);
			if (file != NULL) {
				fd = fd_add_file(file);
				if (fd == -1)
					file_close(file);
			}
			lock_release(&filesys_lock);
			f->R.rax = fd;
			break;
		}

		case SYS_FILESIZE: {

			int fd = (int) f->R.rdi;
			struct file *file = fd_get_file(fd);

			if (file == NULL) {
				f->R.rax = -1;
			} else {
				lock_acquire(&filesys_lock);
				f->R.rax = file_length(file);
				lock_release(&filesys_lock);
			}
			break;
		}

		case SYS_READ: {

			int fd = (int) f->R.rdi;
			void *buffer = (void *) f->R.rsi;
			unsigned size = (unsigned) f->R.rdx;
			struct file *file;

			check_buffer(buffer, size);

			if (fd == 0) {
				unsigned i;
				uint8_t *buf = buffer;

				for (i = 0; i < size; i++)
					buf[i] = input_getc();
				f->R.rax = size;
			} else if (fd == 1) {

				f->R.rax = -1;
			} else {
				file = fd_get_file(fd);
				if (file == NULL) {
					f->R.rax = -1;
				} else {
					lock_acquire(&filesys_lock);
					f->R.rax = file_read(file, buffer, size);
					lock_release(&filesys_lock);
				}
			}
			break;
		}

		case SYS_SEEK: {

			int fd = (int) f->R.rdi;
			unsigned position = (unsigned) f->R.rsi;
			struct file *file = fd_get_file(fd);

			if (file != NULL && position <= INT32_MAX) {
				lock_acquire(&filesys_lock);
				file_seek(file, (off_t) position);
				lock_release(&filesys_lock);
			}
			break;
		}

		case SYS_TELL: {

			int fd = (int) f->R.rdi;
			struct file *file = fd_get_file(fd);

			if (file == NULL) {
				f->R.rax = -1;
			} else {
				lock_acquire(&filesys_lock);
				f->R.rax = file_tell(file);
				lock_release(&filesys_lock);
			}
			break;
		}

		case SYS_CLOSE: {

			int fd = (int) f->R.rdi;

			fd_close_file(fd);
			break;
		}

		default:


			exit_with_status(-1);
			break;
	}
	// printf ("system call!\n");
	// thread_exit ();
}
