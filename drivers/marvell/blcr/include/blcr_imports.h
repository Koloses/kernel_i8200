/* 
 * Berkeley Lab Checkpoint/Restart (BLCR) for Linux is Copyright (c)
 * 2008, The Regents of the University of California, through Lawrence
 * Berkeley National Laboratory (subject to receipt of any required
 * approvals from the U.S. Dept. of Energy).  All rights reserved.
 *
 * Portions may be copyrighted by others, as may be noted in specific
 * copyright notices within specific files.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id: blcr_imports.h.in,v 1.14.14.2 2010/08/12 21:58:34 phargrov Exp $
 *
 * This file provides otherwise missing declarations for functions
 * and data which BLCR imports via the blcr_imports kernel module.
 * These declartations serve two purposes:
 * 1) The import mechanism requires a visible prototype.
 * 2) The code that uses the imported symbols will need prototypes too.
 */
#ifndef _BLCR_IMPORTS_H
#define _BLCR_IMPORTS_H	1

#ifdef CR_NEED_AUTOCONF_H
#include <linux/autoconf.h>
#endif
#if defined(CONFIG_SMP) && ! defined(__SMP__)
  #define __SMP__
#endif
#if defined(CONFIG_MODVERSIONS) && ! defined(MODVERSIONS)
  #define MODVERSIONS
#endif
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/spinlock.h>
#include <asm/atomic.h>
#include <linux/unistd.h>
#include <asm/unistd.h>
#include <asm/processor.h>
#include <linux/ptrace.h>
#include <linux/binfmts.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/fs_struct.h>
#if HAVE_LINUX_SYSCALLS_H
  #include <linux/syscalls.h>
#endif
#if HAVE_LINUX_PSPACE_H
  #include <linux/pspace.h>
#endif
#if HAVE_LINUX_PID_NAMESPACE_H
  #include <linux/pid_namespace.h>
#endif
#if HAVE_ASM_ELF_H
  #include <asm/elf.h>
#endif
#if HAVE_ASM_DESC_H
  #include <asm/desc.h>
#endif
#if HAVE_ASM_I387_H
  #include <asm/i387.h>
#endif
#include <linux/jiffies.h>
#include <linux/hugetlb.h>
#if HAVE_LINUX_UTRACE_H
  #include <linux/utrace.h>
#endif
#if HAVE_LINUX_FDTABLE_H
  #include <linux/fdtable.h>
#endif
#ifndef FASTCALL
  #define FASTCALL(_decl) _decl
#endif

extern const char *blcr_config_timestamp;

#ifndef IN_CONFIGURE
/* BEGIN AUTO-GENERATED FRAGMENT */

/* This file is autogenerated - do not edit or remove */
extern int __kuser_helper_start(void);
extern int __kuser_cmpxchg(void);
extern struct hlist_head *pid_hash;
extern int pidhash_shift;
extern struct pipe_buf_operations anon_pipe_buf_ops;
long pipe_fcntl(struct file *file, unsigned int cmd, unsigned long arg);
extern const struct file_operations shmem_file_operations;
extern const struct file_operations ramfs_file_operations;
/* END AUTO-GENERATED FRAGMENT */
#endif

#endif /* !_BLCR_IMPORTS_H */
