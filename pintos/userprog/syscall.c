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

/* exit_status 저장 후 스레드 종료 */
static void exit_with_status(int status){
	thread_current()->exit_status = status;
	thread_exit();
}

/* 주소 하나가 안전한지 검사 — NULL, 커널 영역, 미매핑 주소면 종료 */
static void check_address(const void *addr) {
	if(addr == NULL || is_user_vaddr(addr) == 0 || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		exit_with_status(-1);
	}
}

/* 버퍼 전체 범위(시작~끝)가 안전한지 검사 */
static void check_buffer(const void *buffer, unsigned size) {
	const char *cur = buffer;
	unsigned i;

	/* 0바이트 read/write는 실제로 버퍼를 건드리지 않으므로 검사 없이 통과한다. */
	if (size == 0)
		return;

	for(i = 0; i < size; i++) {
		check_address(cur + i);
	}
}

/* 문자열이 '\0'까지 전부 안전한지 한 글자씩 검사 */
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

/* 현재 프로세스의 fd 테이블에 파일을 넣고 새 fd 번호를 반환한다.
 * fd 0, 1은 표준 입출력으로 예약되어 있으므로 일반 파일은 2번부터 사용한다. */
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

/* fd 번호가 실제 열린 파일을 가리키는지 확인하고 파일 객체를 반환한다. */
static struct file *
fd_get_file (int fd) {
	struct thread *cur = thread_current ();

	if (fd < 2 || fd >= MAX_FD)
		return NULL;
	return cur->fd_table[fd];
}

/* fd 테이블에서 파일을 제거한 뒤 실제 file 객체를 닫는다. */
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
	/* f->R.rax 에는 유저 프로그램이 요청한 syscall 번호가 들어 있다.
	 * 이 번호를 기준으로 어떤 작업을 할지 분기한다. */
	switch(f->R.rax){
		case SYS_HALT:
			/* halt()는 현재 프로세스만 끝내는 것이 아니라
			 * Pintos 자체를 종료하는 시스템콜이다. */
			power_off();
			break;

		case SYS_EXIT:
			/* exit()는 현재 유저 프로그램을 종료한다. */
			exit_with_status(f->R.rdi);
			break;

		case SYS_WRITE: {
			/* write(fd, buffer, size)의 세 인자는
			 * 첫 번째부터 차례로 rdi, rsi, rdx에 들어온다. */
			int fd = (int) f->R.rdi;
			const void *buffer = (const void *) f->R.rsi;
			unsigned size = (unsigned) f->R.rdx;
			struct file *file;

			// 유저가 넘긴 buffer부터 size 바이트까지 모두 안전한지 검사
			check_buffer (buffer, size);

			if(fd == 1) {
				/* fd == 1은 stdout이므로
				 * 화면(콘솔)에 문자열을 출력한다. */
				putbuf(buffer, size);

				/* syscall 반환값은 rax로 돌려준다.
				 * write 성공 시에는 출력한 바이트 수를 반환한다. */
				f->R.rax = size;
			}
			else if (fd == 0) {
				/* fd 0은 stdin이므로 쓰기 대상이 아니다. */
				f->R.rax = -1;
			}
			else {
				/* 일반 파일 fd라면 현재 파일 위치부터 size 바이트를 쓴다. */
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
			// 1번째 인자(rdi): 생성할 파일 이름 (유저 포인터)
			char *filename = (char *) f->R.rdi;
			// 2번째 인자(rsi): 파일의 초기 크기 (바이트 단위)
			unsigned size = (unsigned) f->R.rsi;

			// filename 포인터가 NULL이거나 커널 주소이거나 매핑 안 된 주소면 -1로 종료
			// check_address(filename);
			check_string(filename);
			// 파일 시스템은 thread-safe하지 않으므로 락을 잡아 직렬화
			lock_acquire(&filesys_lock);
			// 실제 파일 생성 — 성공하면 true, 실패하면 false를 rax로 반환
			f->R.rax = filesys_create(filename, size);
			// 파일 시스템 작업 끝났으니 락 해제
			lock_release(&filesys_lock);

			break;
		}

		case SYS_REMOVE: {
			/* remove(file): 파일 이름이 유저 문자열이므로 먼저 끝까지 검증한다. */
			char *filename = (char *) f->R.rdi;

			check_string(filename);
			lock_acquire(&filesys_lock);
			f->R.rax = filesys_remove(filename);
			lock_release(&filesys_lock);
			break;
		}

		case SYS_OPEN: {
			/* open(file): 열린 file 객체를 현재 프로세스의 fd 테이블에 등록한다. */
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
			/* filesize(fd): fd가 가리키는 파일의 총 길이를 바이트 단위로 반환한다. */
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
			/* read(fd, buffer, size): fd 0은 키보드 입력, 일반 fd는 파일 읽기다. */
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
				/* fd 1은 stdout이므로 읽기 대상이 아니다. */
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
			/* seek(fd, position): 해당 파일의 다음 read/write 위치를 바꾼다. */
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
			/* tell(fd): 현재 파일 위치를 반환한다. */
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
			/* close(fd): fd 테이블에서 제거하고 실제 file 객체를 닫는다. */
			int fd = (int) f->R.rdi;

			fd_close_file(fd);
			break;
		}

		default:
			/* 아직 구현하지 않은 syscall 번호가 들어오면
			 * 우선 현재 프로세스를 종료한다. */
			exit_with_status(-1);
			break;
	}
	// printf ("system call!\n");
	// thread_exit ();
}
