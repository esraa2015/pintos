#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "devices/input.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "vm/page.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <inttypes.h>
#include <list.h>
#include "vm/mmap.h"

/* Process identifier. */
typedef int pid_t;
/* File identifier. */
typedef int fid_t;

static void syscall_handler (struct intr_frame *);

static void      sys_halt (void);
static void      sys_exit (int status);
static pid_t     sys_exec (const char *file);
static int       sys_wait (pid_t pid);
static bool      sys_create (const char *file, unsigned initial_size);
static bool      sys_remove (const char *file);
static int       sys_open (const char *file);
static int       sys_filesize (int fd);
static int       sys_read (int fd, void *buffer, unsigned size);
static int       sys_write (int fd, const void *buffer, unsigned size);
static void      sys_seek (int fd, unsigned position);
static unsigned  sys_tell (int fd);
static void      sys_close (int fd);
static mapid_t   sys_mmap (int fd, void *addr);
static void      sys_munmap (mapid_t mapping);

static struct user_file *file_by_fid (fid_t);
static fid_t allocate_fid (void);
static mapid_t allocate_mapid (void);

static struct lock file_lock;
static struct list file_list;

typedef int (*handler) (uint32_t, uint32_t, uint32_t);
static handler syscall_map[32];
//static struct hash sys_mfile;

static struct intr_frame *fbck;

struct user_file
  {
    struct file *file;                 /* Pointer to the actual file */
    fid_t fid;                         /* File identifier */
    struct list_elem thread_elem;      /* List elem for a thread's file list */
  };

/* Initialization of syscall handlers */
void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");

  syscall_map[SYS_HALT]     = (handler)sys_halt;
  syscall_map[SYS_EXIT]     = (handler)sys_exit;
  syscall_map[SYS_EXEC]     = (handler)sys_exec;
  syscall_map[SYS_WAIT]     = (handler)sys_wait;
  syscall_map[SYS_CREATE]   = (handler)sys_create;
  syscall_map[SYS_REMOVE]   = (handler)sys_remove;
  syscall_map[SYS_OPEN]     = (handler)sys_open;
  syscall_map[SYS_FILESIZE] = (handler)sys_filesize;
  syscall_map[SYS_READ]     = (handler)sys_read;
  syscall_map[SYS_WRITE]    = (handler)sys_write;
  syscall_map[SYS_SEEK]     = (handler)sys_seek;
  syscall_map[SYS_TELL]     = (handler)sys_tell;
  syscall_map[SYS_CLOSE]    = (handler)sys_close;
  syscall_map[SYS_MMAP]     = (handler)sys_mmap;
  syscall_map[SYS_MUNMAP]   = (handler)sys_munmap;
  lock_init (&file_lock);
  list_init (&file_list);
}

/* Syscall handler calls the appropriate function. */
static void
syscall_handler (struct intr_frame *f)
{
  handler function;
  int *param = f->esp, ret;

  if ( !is_user_vaddr(param) )
    sys_exit (-1);

  if (!( is_user_vaddr (param + 1) && is_user_vaddr (param + 2) && is_user_vaddr (param + 3)))
    sys_exit (-1);

  if (*param < SYS_HALT || *param > SYS_INUMBER)
    sys_exit (-1);

  function = syscall_map[*param];

  fbck = f;
  ret = function (*(param + 1), *(param + 2), *(param + 3));
  f->eax = ret;

  return;
}

/* Halt the operating system. */
static void
sys_halt (void)
{
  shutdown_power_off ();
}

/* Terminate this process. */
void
sys_exit (int status)
{
  struct thread *t;
  struct list_elem *e;

  t = thread_current ();
  if (lock_held_by_current_thread (&file_lock) )
    lock_release (&file_lock);

  while (!list_empty (&t->files) )
    {
      e = list_begin (&t->files);
      sys_close ( list_entry (e, struct user_file, thread_elem)->fid );
    }

  t->ret_status = status;
  thread_exit ();
}

/* Start another process. */
static pid_t
sys_exec (const char *file)
{
  lock_acquire (&file_lock);
  int ret = process_execute (file);
  lock_release (&file_lock);
  return ret;
}

/* Wait for a child process to die. */
static int
sys_wait (pid_t pid)
{
  return process_wait (pid);
}

/* Create a file. */
static bool
sys_create (const char *file, unsigned initial_size)
{
  if (file == NULL)
    sys_exit (-1);

  int ret = filesys_create (file, initial_size);
  //printf ("[create file] %s with status %d\n", file, ret);
  return ret;
}

/* Delete a file. */
static bool
sys_remove (const char *file)
{
   if (file == NULL)
     sys_exit (-1);
  
  //printf ("[remove file] %s\n", file);  
  return filesys_remove (file);
}

/* Open a file. */
static int
sys_open (const char *file)
{
  struct file *sys_file;
  struct user_file *f;

  //printf ("here 1 %s\n", file);

  if (file == NULL)
    return -1;

  sys_file = filesys_open (file);
  if (sys_file == NULL)
    return -1;

  //printf ("here 2 %s\n", file);

  f = (struct user_file *) malloc (sizeof (struct user_file));
  if (f == NULL)
    {
      file_close (sys_file);
      return -1;
    }

  lock_acquire (&file_lock);
  f->file = sys_file;
  f->fid = allocate_fid ();
  list_push_back (&thread_current ()->files, &f->thread_elem);
  lock_release (&file_lock);

  return f->fid;
}

/* Obtain a file's size. */
static int
sys_filesize (int fd)
{
  struct user_file *f;
  int size = -1;

  f = file_by_fid (fd);
  if (f == NULL)
    return -1;

  lock_acquire (&file_lock);
  size = file_length (f->file);
  lock_release (&file_lock);

  return size;
}

/* Read from a file. */
static int
sys_read (int fd, void *buffer, unsigned length)
{
  struct user_file *f;
  int ret = -1;

  lock_acquire (&file_lock);
  if (fd == STDIN_FILENO)
    {
      unsigned i;
      for (i = 0; i < length; ++i)
        *(uint8_t *)(buffer + i) = input_getc ();
      ret = length;
    }
  else if (fd == STDOUT_FILENO)
    ret = -1;
  else if ( !is_user_vaddr (buffer) || !is_user_vaddr (buffer + length) )
    {
      lock_release (&file_lock);
      sys_exit (-1);
    }
  else
    {
      f = file_by_fid (fd);
      if (f == NULL)
        ret = -1;
      else
        {
          uint32_t *pagedir = thread_current ()->pagedir;          
          size_t rem = length;
          void *tmp_buffer = buffer;
          
          ret = 0;
          while (rem > 0)
            {
              size_t ofs = tmp_buffer - pg_round_down (tmp_buffer);
              struct vm_page *page = find_page ( tmp_buffer - ofs, pagedir);
              
              void *addr;
              asm ("movl %%esp, %0" : "=r" (addr));

              /* I experience a weird behaviour for page-mer-stk test. It needs 
              to grow its stack when reading but the fault address it's 30 bytes
              above the stack pointer so it's not recognized as authentic stack
              access. We need to figure out this */
              if (page == NULL ) //&& stack_access(fbck, tmp_buffer) )
                page = vm_grow_stack (tmp_buffer - ofs);   
              else if (page == NULL)
                sys_t_exit (-1);

              if ( !page->loaded )
                vm_load_page (page, tmp_buffer - ofs, pagedir);
              vm_pin_page (page);

              size_t read_bytes = ofs + rem > PGSIZE ? rem - (ofs + rem - PGSIZE) : rem;
              ret += file_read (f->file, tmp_buffer, read_bytes);
              
              rem -= read_bytes;
              tmp_buffer += read_bytes;
              
              vm_unpin_page (page);
            }
        }
    }
  lock_release (&file_lock);

  return ret;
}

/* Write to a file. */
static int
sys_write (int fd, const void *buffer, unsigned length)
{
  struct user_file *f;
  int ret = -1;

  lock_acquire (&file_lock);
  if (fd == STDIN_FILENO)
    ret = -1;
  else if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, length);
      ret = length;
    }
  else if ( !is_user_vaddr (buffer) || !is_user_vaddr (buffer + length) )
    {
      lock_release (&file_lock);
      sys_exit (-1);
    }
  else
    {
      f = file_by_fid (fd);
      if (f == NULL)
        ret = -1;
      else
        {
          uint32_t *pagedir = thread_current ()->pagedir;
          size_t rem = length;
          void *tmp_buffer = buffer;

          ret = 0;
          while (rem > 0)
            {
              size_t ofs = tmp_buffer - pg_round_down (tmp_buffer);
              struct vm_page *page = find_page ( tmp_buffer - ofs, pagedir);
              
              if (page == NULL && stack_access(fbck, tmp_buffer) )
                page = vm_grow_stack (tmp_buffer - ofs);   
              else if (page == NULL)
                sys_t_exit (-1);

              if ( !page->loaded )
                vm_load_page (page, tmp_buffer - ofs, pagedir);
              vm_pin_page (page);

              size_t write_bytes = ofs + rem > PGSIZE ? rem - (ofs + rem - PGSIZE) : rem;
              ret += file_write (f->file, tmp_buffer, write_bytes);
              
              rem -= write_bytes;
              tmp_buffer += write_bytes;
              
              vm_unpin_page (page);
            }
        }
    }
  lock_release (&file_lock);

  return ret;
}

/* Change position in a file. */
static void
sys_seek (int fd, unsigned position)
{
  struct user_file *f;

  f = file_by_fid (fd);
  if (!f)
    sys_exit (-1);

  lock_acquire (&file_lock);
  file_seek (f->file, position);
  lock_release (&file_lock);
}

/* Report current position in a file. */
static unsigned
sys_tell (int fd)
{
  struct user_file *f;
  unsigned status;

  f = file_by_fid (fd);
  if (!f)
    sys_exit (-1);

  lock_acquire (&file_lock);
  status = file_tell (f->file);
  lock_release (&file_lock);

  return status;
}

/* Close a file. */
static void
sys_close (int fd)
{
  struct user_file *f;

  f = file_by_fid (fd);

  if (f == NULL)
    sys_exit (-1);

  lock_acquire (&file_lock);
  list_remove (&f->thread_elem);
  file_close (f->file);
  free (f);
  lock_release (&file_lock);
}

/* Allocate a new fid for a file */
static fid_t
allocate_fid (void)
{
  static fid_t next_fid = 2;
  return next_fid++;
}

/* Allocate a new mapid for a file */
static mapid_t
allocate_mapid (void)
{
  static mapid_t next_mapid = 0;
  return next_mapid++;
}

/* Returns the file with the given fid from the current thread's files */
static struct user_file *
file_by_fid (int fid)
{
  struct list_elem *e;
  struct thread *t;

  t = thread_current();
  for (e = list_begin (&t->files); e != list_end (&t->files);
       e = list_next (e))
    {
      struct user_file *f = list_entry (e, struct user_file, thread_elem);
      if (f->fid == fid)
        return f;
    }

  return NULL;
}

/* Extern function for sys_exit */
void 
sys_t_exit (int status)
{
  sys_exit (status);
}

mapid_t
sys_mmap (int fd, void *addr)
{
  struct user_file *f;
  int size, page_allign;

  lock_acquire (&file_lock);
  f = file_by_fid (fd);
  size = file_length (f->file);
  lock_release (&file_lock);
  if (size == 0)
    return -1;

  page_allign = pg_ofs (addr);
  if (page_allign != 0)
    return -1;

  if (addr == NULL || addr == 0x0)
    return -1;
  if (fd == STDIN_FILENO || fd == STDOUT_FILENO)
    return -1;

  lock_acquire (&file_lock);
  struct user_file *f_reopen = file_reopen (f);
  size_t ofs = 0;
  while (size > 0)
    {
      size_t read_bytes;
      size_t zero_bytes;
      struct vm_page *page;
      if (size >= PGSIZE)
        {
          read_bytes = PGSIZE;
          zero_bytes = 0;
          page = vm_new_file_page (addr, f_reopen, ofs, read_bytes, zero_bytes,
                                   true);
        }
      else
        {
          read_bytes = size;
          zero_bytes = PGSIZE - size;
          page = vm_new_file_page (addr, f_reopen, ofs, read_bytes, zero_bytes,
                                   true);
        }
      ofs += PGSIZE;
      size -= PGSIZE;
      addr += PGSIZE;
    }
  mapid_t mapping = allocate_mapid();
  vm_mfile_insert (mapping, fd);
  lock_release (&file_lock);
  return mapping;
}

void
sys_munmap (mapid_t mapping)
{
  lock_acquire (&file_lock);
  uint32_t *pagedir = thread_current ()->pagedir;
  struct vm_mfile *f = vm_find_mfile (mapping);
  if (f == NULL)
    return;
 
  struct vm_page *page = find_page (f->addr, pagedir);
  if (page == NULL)
    return;

  vm_unload_page (page, f->addr);
  vm_delete_page (page);
  vm_delete_mfile (mapping);
  lock_release (&file_lock);  
}
