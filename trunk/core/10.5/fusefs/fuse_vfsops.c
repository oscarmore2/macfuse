/*
 * Copyright (C) 2006-2007 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#include "fuse.h"
#include "fuse_device.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_kludges.h"
#include "fuse_locking.h"
#include "fuse_node.h"
#include "fuse_sysctl.h"
#include "fuse_vfsops.h"

#include <fuse_mount.h>

static const struct timespec kZeroTime = { 0, 0 };

vfstable_t fuse_vfs_table_ref = NULL;

errno_t (**fuse_vnode_operations)(void *);

static struct vnodeopv_desc fuse_vnode_operation_vector_desc = {
    &fuse_vnode_operations,      // opv_desc_vector_p
    fuse_vnode_operation_entries // opv_desc_ops
};

#if M_MACFUSE_ENABLE_FIFOFS
errno_t (**fuse_fifo_operations)(void *);

static struct vnodeopv_desc fuse_fifo_operation_vector_desc = {
    &fuse_fifo_operations,      // opv_desc_vector_p
    fuse_fifo_operation_entries // opv_desc_ops
};
#endif /* M_MACFUSE_ENABLE_FIFOFS */

#if M_MACFUSE_ENABLE_SPECFS
errno_t (**fuse_spec_operations)(void *);

static struct vnodeopv_desc fuse_spec_operation_vector_desc = {
    &fuse_spec_operations,      // opv_desc_vector_p
    fuse_spec_operation_entries // opv_desc_ops
};
#endif /* M_MACFUSE_ENABLE_SPECFS */

static struct vnodeopv_desc *fuse_vnode_operation_vector_desc_list[] =
{
    &fuse_vnode_operation_vector_desc,

#if M_MACFUSE_ENABLE_FIFOFS
    &fuse_fifo_operation_vector_desc,
#endif

#if M_MACFUSE_ENABLE_SPECFS
    &fuse_spec_operation_vector_desc,
#endif
};

static struct vfsops fuse_vfs_ops = {
    fuse_vfs_mount,   // vfs_mount
    NULL,             // vfs_start
    fuse_vfs_unmount, // vfs_unmount
    fuse_vfs_root,    // vfs_root
    NULL,             // vfs_quotactl
    fuse_vfs_getattr, // vfs_getattr
    fuse_vfs_sync,    // vfs_sync
    NULL,             // vfs_vget
    NULL,             // vfs_fhtovp
    NULL,             // vfs_vptofh
    NULL,             // vfs_init
    NULL,             // vfs_sysctl
    fuse_vfs_setattr, // vfs_setattr
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL } // vfs_reserved[]
};

struct vfs_fsentry fuse_vfs_entry = {

    // VFS operations
    &fuse_vfs_ops,

    // Number of vnodeopv_desc being registered
    (sizeof(fuse_vnode_operation_vector_desc_list) /\
        sizeof(*fuse_vnode_operation_vector_desc_list)),

    // The vnodeopv_desc's
    fuse_vnode_operation_vector_desc_list,

    // File system type number
    0,

    // File system type name
    MACFUSE_FS_TYPE,

    // Flags specifying file system capabilities
    VFS_TBL64BITREADY | VFS_TBLNOTYPENUM,

    // Reserved for future use
    { NULL, NULL }
};

static errno_t
fuse_vfs_mount(mount_t mp, __unused vnode_t devvp, user_addr_t udata,
               __unused vfs_context_t context)
{
    int err     = 0;
    int mntopts = 0;
    int mounted = 0;

    uint32_t drandom  = 0;
    uint32_t max_read = ~0;

    size_t len;

    fuse_device_t      fdev = FUSE_DEVICE_NULL;
    struct fuse_data  *data = NULL;
    fuse_mount_args    fusefs_args;

    fuse_trace_printf_vfsop();

    if (vfs_isupdate(mp)) {
        return ENOTSUP;
    }

    err = copyin(udata, &fusefs_args, sizeof(fusefs_args));
    if (err) {
        return EINVAL;
    }

    /*
     * Interesting flags that we can receive from mount or may want to
     * otherwise forcibly set include:
     *
     *     MNT_ASYNC
     *     MNT_AUTOMOUNTED              
     *     MNT_DEFWRITE
     *     MNT_DONTBROWSE
     *     MNT_IGNORE_OWNERSHIP
     *     MNT_JOURNALED
     *     MNT_NODEV
     *     MNT_NOEXEC
     *     MNT_NOSUID
     *     MNT_NOUSERXATTR
     *     MNT_RDONLY
     *     MNT_SYNCHRONOUS
     *     MNT_UNION
     */

    err = ENOTSUP;

#if M_MACFUSE_ENABLE_LOCKLOCAL
    vfs_setlocklocal(mp);
#endif

    /** Option Processing. **/


    if ((fusefs_args.daemon_timeout > FUSE_MAX_DAEMON_TIMEOUT) ||
        (fusefs_args.daemon_timeout < FUSE_MIN_DAEMON_TIMEOUT)) {
        return EINVAL;
    }

    if ((fusefs_args.init_timeout > FUSE_MAX_INIT_TIMEOUT) ||
        (fusefs_args.init_timeout < FUSE_MIN_INIT_TIMEOUT)) {
        return EINVAL;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_ALERTS) {
        mntopts |= FSESS_NO_ALERTS;
    }

    if (fusefs_args.altflags & FUSE_MOPT_AUTO_XATTR) {
        mntopts |= FSESS_AUTO_XATTR;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_BROWSE) {
        vfs_setflags(mp, MNT_DONTBROWSE);
    }

    if (fusefs_args.altflags & FUSE_MOPT_JAIL_SYMLINKS) {
        mntopts |= FSESS_JAIL_SYMLINKS;
    }

    /*
     * Note that unlike Linux, which keeps allow_root in user-space and
     * passes allow_other in that case to the kernel, we let allow_root
     * reach the kernel. The 'if' ordering is important here.
     */
    if (fusefs_args.altflags & FUSE_MOPT_ALLOW_ROOT) {
        int is_member = 0;
        if ((kauth_cred_ismember_gid(kauth_cred_get(), fuse_admin_group,
                                    &is_member) == 0) && is_member) {
            mntopts |= FSESS_ALLOW_ROOT;
        } else {
            IOLog("MacFUSE: caller not a member of MacFUSE admin group (%d)\n",
                  fuse_admin_group);
            return EPERM;
        }
    } else if (fusefs_args.altflags & FUSE_MOPT_ALLOW_OTHER) {
        if (!fuse_allow_other && !fuse_vfs_context_issuser(context)) {
            return EPERM;
        }
        mntopts |= FSESS_ALLOW_OTHER;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_APPLEDOUBLE) {
        mntopts |= FSESS_NO_APPLEDOUBLE;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_APPLEXATTR) {
        mntopts |= FSESS_NO_APPLEXATTR;
    }

    if ((fusefs_args.altflags & FUSE_MOPT_FSID) && (fusefs_args.fsid != 0)) {
        fsid_t   fsid;
        mount_t  other_mp;
        uint32_t target_dev;

        target_dev = FUSE_MAKEDEV(FUSE_CUSTOM_FSID_DEVICE_MAJOR,
                                  fusefs_args.fsid);

        fsid.val[0] = target_dev;
        fsid.val[1] = FUSE_CUSTOM_FSID_VAL1;

        other_mp = vfs_getvfs(&fsid);
        if (other_mp != NULL) {
            err = EPERM;
            goto out;
        }

        vfs_statfs(mp)->f_fsid.val[0] = target_dev;
        vfs_statfs(mp)->f_fsid.val[1] = FUSE_CUSTOM_FSID_VAL1;

    } else {
        vfs_getnewfsid(mp);    
    }

    if (fusefs_args.altflags & FUSE_MOPT_KILL_ON_UNMOUNT) {
        mntopts |= FSESS_KILL_ON_UNMOUNT;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_ATTRCACHE) {
        mntopts |= FSESS_NO_ATTRCACHE;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_READAHEAD) {
        mntopts |= FSESS_NO_READAHEAD;
    }

    if (fusefs_args.altflags & (FUSE_MOPT_NO_UBC | FUSE_MOPT_DIRECT_IO)) {
        mntopts |= FSESS_NO_UBC;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_VNCACHE) {
        if (fusefs_args.altflags & FUSE_MOPT_EXTENDED_SECURITY) {
            /* 'novncache' and 'extended_security' don't mix well. */
            return EINVAL;
        }
        mntopts |= FSESS_NO_VNCACHE;
        mntopts |= (FSESS_NO_ATTRCACHE | FSESS_NO_READAHEAD | FSESS_NO_UBC);
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_LOCALCACHES) {
        fusefs_args.altflags |= FUSE_MOPT_NO_READAHEAD;
        fusefs_args.altflags |= FUSE_MOPT_NO_UBC;
        fusefs_args.altflags |= FUSE_MOPT_NO_VNCACHE;
    }

    if (mntopts & FSESS_NO_UBC) {
        /* If no buffer cache, disallow exec from file system. */
        vfs_setflags(mp, MNT_NOEXEC);
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_SYNCWRITES) {

        /* Cannot mix 'nosyncwrites' with 'noubc' or 'noreadahead'. */
        if (fusefs_args.altflags &
            (FUSE_MOPT_NO_UBC | FUSE_MOPT_NO_READAHEAD)) {
            return EINVAL;
        }

        mntopts |= FSESS_NO_SYNCWRITES;
        vfs_clearflags(mp, MNT_SYNCHRONOUS);
        vfs_setflags(mp, MNT_ASYNC);

        /* We check for this only if we have nosyncwrites in the first place. */
        if (fusefs_args.altflags & FUSE_MOPT_NO_SYNCONCLOSE) {
            mntopts |= FSESS_NO_SYNCONCLOSE;
        }

    } else {
        vfs_clearflags(mp, MNT_ASYNC);
        vfs_setflags(mp, MNT_SYNCHRONOUS);
    }

    err = 0;

    vfs_setauthopaque(mp);
    vfs_setauthopaqueaccess(mp);

    if ((fusefs_args.altflags & FUSE_MOPT_DEFAULT_PERMISSIONS) &&
        (fusefs_args.altflags & FUSE_MOPT_DEFER_PERMISSIONS)) {
        return EINVAL;
    }

    if (fusefs_args.altflags & FUSE_MOPT_DEFAULT_PERMISSIONS) {
        mntopts |= FSESS_DEFAULT_PERMISSIONS;
        vfs_clearauthopaque(mp);
    }

    if (fusefs_args.altflags & FUSE_MOPT_DEFER_PERMISSIONS) {
        mntopts |= FSESS_DEFER_PERMISSIONS;
    }

    if (fusefs_args.altflags & FUSE_MOPT_EXTENDED_SECURITY) {
        mntopts |= FSESS_EXTENDED_SECURITY;
        vfs_setextendedsecurity(mp);
    }

    vfs_setfsprivate(mp, NULL);

    fdev = fuse_device_get(fusefs_args.rdev);
    if (!fdev) {
        return EINVAL;
    }

    fuse_device_lock(fdev);

    drandom = fuse_device_get_random(fdev);
    if (fusefs_args.random != drandom) {
        fuse_device_unlock(fdev);
        IOLog("MacFUSE: failing mount because of mismatched random\n");
        return EINVAL; 
    }

    data = fuse_device_get_mpdata(fdev);

    if (!data) {
        fuse_device_unlock(fdev);
        return ENXIO;
    }

    if (data->mount_state != FM_NOTMOUNTED) {
        fuse_device_unlock(fdev);
        return EALREADY;
    }

    if (!(data->dataflags & FSESS_OPENED)) {
        fuse_device_unlock(fdev);
        err = ENXIO;
        goto out;
    }

    data->mount_state = FM_MOUNTED;
    OSAddAtomic(1, (SInt32 *)&fuse_mount_count);
    mounted = 1;

    if (fdata_dead_get(data)) {
        fuse_device_unlock(fdev);
        err = ENOTCONN;
        goto out;
    }

    if (!data->daemoncred) {
        panic("MacFUSE: daemon found but identity unknown");
    }

    if (fuse_vfs_context_issuser(context) &&
        vfs_context_ucred(context)->cr_uid != data->daemoncred->cr_uid) {
        fuse_device_unlock(fdev);
        err = EPERM;
        goto out;
    }

    data->mp = mp;
    data->fdev = fdev;
    data->dataflags |= mntopts;

    data->daemon_timeout.tv_sec =  fusefs_args.daemon_timeout;
    data->daemon_timeout.tv_nsec = 0;
    if (data->daemon_timeout.tv_sec) {
        data->daemon_timeout_p = &(data->daemon_timeout);
    } else {
        data->daemon_timeout_p = (struct timespec *)0;
    }

    data->init_timeout.tv_sec = fusefs_args.init_timeout;
    data->init_timeout.tv_nsec = 0;

    data->max_read = max_read;
    data->fssubtype = fusefs_args.fssubtype;
    data->mountaltflags = fusefs_args.altflags;
    data->noimplflags = (uint64_t)0;

    data->blocksize = fuse_round_size(fusefs_args.blocksize,
                                      FUSE_MIN_BLOCKSIZE, FUSE_MAX_BLOCKSIZE);

    data->iosize = fuse_round_size(fusefs_args.iosize,
                                   FUSE_MIN_IOSIZE, FUSE_MAX_IOSIZE);

    if (data->iosize < data->blocksize) {
        data->iosize = data->blocksize;
    }

    copystr(fusefs_args.fsname, vfs_statfs(mp)->f_mntfromname,
            MNAMELEN - 1, &len);
    bzero(vfs_statfs(mp)->f_mntfromname + len, MNAMELEN - len);

    copystr(fusefs_args.volname, data->volname, MAXPATHLEN - 1, &len);
    bzero(data->volname + len, MAXPATHLEN - len);

    vfs_setfsprivate(mp, data);

    fuse_device_unlock(fdev);

    /* Handshake with the daemon. Blocking. */
    err = fuse_internal_send_init(data, context);

out:
    if (err) {
        vfs_setfsprivate(mp, NULL);

        fuse_device_lock(fdev);
        data = fuse_device_get_mpdata(fdev); /* again */
        if (mounted) {
            OSAddAtomic(-1, (SInt32 *)&fuse_mount_count);
        }
        if (data) {
            data->mount_state = FM_NOTMOUNTED;
            if (!(data->dataflags & FSESS_OPENED)) {
                fuse_device_close_final(fdev);
                /* data is gone now */
            }
        }
        fuse_device_unlock(fdev);
    } else {
        vnode_t rootvp = NULLVP;
        err = fuse_vfs_root(mp, &rootvp, context);
        if (err) {
            goto out; /* go back and follow error path */
        }
        err = vnode_ref(rootvp);
        (void)vnode_put(rootvp);
        if (err) {
            goto out; /* go back and follow error path */
        }
    }

    return (err);
}

static errno_t
fuse_vfs_unmount(mount_t mp, int mntflags, vfs_context_t context)
{
    int   err        = 0;
    int   flags      = 0;
    int   needsignal = 0;
    pid_t daemonpid  = 0;

    fuse_device_t          fdev;
    struct fuse_data      *data;
    struct fuse_dispatcher fdi;

    vnode_t rootvp = NULLVP;

    fuse_trace_printf_vfsop();

    if (mntflags & MNT_FORCE) {
        flags |= FORCECLOSE;
    }

    data = fuse_get_mpdata(mp);
    if (!data) {
        panic("MacFUSE: no mount private data in vfs_unmount");
    }

    fdev = data->fdev;

    if (fdata_dead_get(data)) {
        /*
         * If the file system daemon is dead, it's pointless to try to do
         * any unmount-time operations that go out to user space. Therefore,
         * we pretend that this is a force unmount. However, this isn't of much
         * use. That's because if any non-root vnode is in use, the vflush()
         * that the kernel does before calling our VFS_UNMOUNT will fail
         * if the original unmount wasn't forcible already. That earlier
         * vflush is called with SKIPROOT though, so it wouldn't bail out
         * on the root vnode being in use. That's the only case where this
         * the following FORCECLOSE will come in. Maybe I should just not do
         * it as this might cause confusion. Let us see. I'll revisit this.
         */
        flags |= FORCECLOSE;
        IOLog("MacFUSE: forcing unmount on dead file system\n");
    } else if (!(data->dataflags & FSESS_INITED)) {
        flags |= FORCECLOSE;
        IOLog("MacFUSE: forcing unmount on not-yet-alive file system\n");
        fdata_set_dead(data);
    }

    rootvp = data->rootvp;

    err = vflush(mp, rootvp, flags);
    if (err) {
        return (err);
    }

    if (vnode_isinuse(rootvp, 1) && !(flags & FORCECLOSE)) {
        return EBUSY;
    }

    if (fdata_dead_get(data)) {
        goto alreadydead;
    }

    fdisp_init(&fdi, 0 /* no data to send along */);
    fdisp_make(&fdi, FUSE_DESTROY, mp, FUSE_ROOT_ID, context);

    err = fdisp_wait_answ(&fdi);
    if (!err) {
        fuse_ticket_drop(fdi.tick);
    }

    /*
     * Note that dounmount() signals a VQ_UNMOUNT VFS event.
     */

    fdata_set_dead(data);

alreadydead:

    needsignal = data->dataflags & FSESS_KILL_ON_UNMOUNT;
    daemonpid = data->daemonpid;

    vnode_rele(rootvp); /* We got this reference in fuse_vfs_mount(). */

    data->rootvp = NULLVP;

    (void)vflush(mp, NULLVP, FORCECLOSE);

    fuse_device_lock(fdev);

    vfs_setfsprivate(mp, NULL);
    data->mount_state = FM_NOTMOUNTED;
    OSAddAtomic(-1, (SInt32 *)&fuse_mount_count);

    if (!(data->dataflags & FSESS_OPENED)) {

        /* fdev->data was left for us to clean up */

        fuse_device_close_final(fdev);

        /* fdev->data is gone now */
    }

    fuse_device_unlock(fdev);

    if (daemonpid && needsignal) {
        proc_signal(daemonpid, FUSE_POSTUNMOUNT_SIGNAL);
    }

    return (0);
}        

static errno_t
fuse_vfs_root(mount_t mp, struct vnode **vpp, vfs_context_t context)
{
    int err = 0;
    vnode_t vp = NULLVP;
    struct fuse_entry_out feo_root;
    struct fuse_data *data = fuse_get_mpdata(mp);

    fuse_trace_printf_vfsop();

    if (data->rootvp != NULLVP) {
        *vpp = data->rootvp;
        return (vnode_get(*vpp));
    }

    bzero(&feo_root, sizeof(feo_root));
    feo_root.nodeid      = FUSE_ROOT_ID;
    feo_root.generation  = 0;
    feo_root.attr.ino    = FUSE_ROOT_ID;
    feo_root.attr.size   = FUSE_ROOT_SIZE;
    feo_root.attr.mode   = VTTOIF(VDIR);

    err = FSNodeGetOrCreateFileVNodeByID(&vp, FN_IS_ROOT, &feo_root, mp,
                                         NULLVP /* dvp */, context,
                                         NULL /* oflags */);
    *vpp = vp;

    if (!err) {
        data->rootvp = *vpp;
    }

    return (err);
}

static void
handle_capabilities_and_attributes(mount_t mp, struct vfs_attr *attr)
{

    struct fuse_data *data = fuse_get_mpdata(mp);
    if (!data) {
        panic("MacFUSE: no private data for mount point?");
    }

    attr->f_capabilities.capabilities[VOL_CAPABILITIES_FORMAT] = 0
//      | VOL_CAP_FMT_PERSISTENTOBJECTIDS
        | VOL_CAP_FMT_SYMBOLICLINKS

        /*
         * Note that we don't really have hard links in a MacFUSE file system
         * unless the user file system daemon provides persistent/consistent
         * inode numbers. Maybe instead of returning the "wrong" answer here
         * we should just deny knowledge of this capability in the valid bits
         * below.
         */
        | VOL_CAP_FMT_HARDLINKS
//      | VOL_CAP_FMT_JOURNAL
//      | VOL_CAP_FMT_JOURNAL_ACTIVE
        | VOL_CAP_FMT_NO_ROOT_TIMES
//      | VOL_CAP_FMT_SPARSE_FILES
//      | VOL_CAP_FMT_ZERO_RUNS
        | VOL_CAP_FMT_CASE_SENSITIVE
        | VOL_CAP_FMT_CASE_PRESERVING
//      | VOL_CAP_FMT_FAST_STATFS
        | VOL_CAP_FMT_2TB_FILESIZE
//      | VOL_CAP_FMT_OPENDENYMODES
//      | VOL_CAP_FMT_HIDDEN_FILES
//      | VOL_CAP_FMT_PATH_FROM_ID
        ;
    attr->f_capabilities.valid[VOL_CAPABILITIES_FORMAT] = 0
        | VOL_CAP_FMT_PERSISTENTOBJECTIDS
        | VOL_CAP_FMT_SYMBOLICLINKS
        | VOL_CAP_FMT_HARDLINKS
        | VOL_CAP_FMT_JOURNAL
        | VOL_CAP_FMT_JOURNAL_ACTIVE
        | VOL_CAP_FMT_NO_ROOT_TIMES
        | VOL_CAP_FMT_SPARSE_FILES
        | VOL_CAP_FMT_ZERO_RUNS
        | VOL_CAP_FMT_CASE_SENSITIVE
        | VOL_CAP_FMT_CASE_PRESERVING
        | VOL_CAP_FMT_FAST_STATFS
        | VOL_CAP_FMT_2TB_FILESIZE
        | VOL_CAP_FMT_OPENDENYMODES
        | VOL_CAP_FMT_HIDDEN_FILES
        | VOL_CAP_FMT_PATH_FROM_ID
        ;
    attr->f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] = 0
//      | VOL_CAP_INT_SEARCHFS
//      | VOL_CAP_INT_ATTRLIST
//      | VOL_CAP_INT_NFSEXPORT
//      | VOL_CAP_INT_READDIRATTR
//      | VOL_CAP_INT_EXCHANGEDATA
//      | VOL_CAP_INT_COPYFILE
//      | VOL_CAP_INT_ALLOCATE
//      | VOL_CAP_INT_VOL_RENAME
        | VOL_CAP_INT_ADVLOCK
        | VOL_CAP_INT_FLOCK
        | VOL_CAP_INT_EXTENDED_SECURITY
//      | VOL_CAP_INT_USERACCESS
//      | VOL_CAP_INT_MANLOCK
        | VOL_CAP_INT_EXTENDED_ATTR
//      | VOL_CAP_INT_NAMEDSTREAMS
        ;

    if (data->dataflags & FSESS_VOL_RENAME) {
        attr->f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] |=
            VOL_CAP_INT_VOL_RENAME;
    }

    attr->f_capabilities.valid[VOL_CAPABILITIES_INTERFACES] = 0
        | VOL_CAP_INT_SEARCHFS
        | VOL_CAP_INT_ATTRLIST
        | VOL_CAP_INT_NFSEXPORT
        | VOL_CAP_INT_READDIRATTR
        | VOL_CAP_INT_EXCHANGEDATA
        | VOL_CAP_INT_COPYFILE
        | VOL_CAP_INT_ALLOCATE
        | VOL_CAP_INT_VOL_RENAME
        | VOL_CAP_INT_ADVLOCK
        | VOL_CAP_INT_FLOCK
        | VOL_CAP_INT_EXTENDED_SECURITY
        | VOL_CAP_INT_USERACCESS
        | VOL_CAP_INT_MANLOCK
        | VOL_CAP_INT_EXTENDED_ATTR
        | VOL_CAP_INT_NAMEDSTREAMS
        ;

    attr->f_capabilities.capabilities[VOL_CAPABILITIES_RESERVED1] = 0;
    attr->f_capabilities.valid[VOL_CAPABILITIES_RESERVED1] = 0;
    attr->f_capabilities.capabilities[VOL_CAPABILITIES_RESERVED2] = 0;
    attr->f_capabilities.valid[VOL_CAPABILITIES_RESERVED2] = 0;
    VFSATTR_SET_SUPPORTED(attr, f_capabilities);
    
    attr->f_attributes.validattr.commonattr = 0
        | ATTR_CMN_NAME
        | ATTR_CMN_DEVID
        | ATTR_CMN_FSID
        | ATTR_CMN_OBJTYPE
//      | ATTR_CMN_OBJTAG
        | ATTR_CMN_OBJID
//      | ATTR_CMN_OBJPERMANENTID
        | ATTR_CMN_PAROBJID
//      | ATTR_CMN_SCRIPT
//      | ATTR_CMN_CRTIME
//      | ATTR_CMN_MODTIME
//      | ATTR_CMN_CHGTIME
//      | ATTR_CMN_ACCTIME
//      | ATTR_CMN_BKUPTIME
//      | ATTR_CMN_FNDRINFO
        | ATTR_CMN_OWNERID
        | ATTR_CMN_GRPID
        | ATTR_CMN_ACCESSMASK
//      | ATTR_CMN_FLAGS
//      | ATTR_CMN_USERACCESS
        | ATTR_CMN_EXTENDED_SECURITY
//      | ATTR_CMN_UUID
//      | ATTR_CMN_GRPUUID
//      | ATTR_CMN_FILEID
//      | ATTR_CMN_PARENTID
        ;
    attr->f_attributes.validattr.volattr = 0
        | ATTR_VOL_FSTYPE
        | ATTR_VOL_SIGNATURE
        | ATTR_VOL_SIZE
        | ATTR_VOL_SPACEFREE
        | ATTR_VOL_SPACEAVAIL
//      | ATTR_VOL_MINALLOCATION
//      | ATTR_VOL_ALLOCATIONCLUMP
        | ATTR_VOL_IOBLOCKSIZE
//      | ATTR_VOL_OBJCOUNT
        | ATTR_VOL_FILECOUNT
//      | ATTR_VOL_DIRCOUNT
//      | ATTR_VOL_MAXOBJCOUNT
        | ATTR_VOL_MOUNTPOINT
        | ATTR_VOL_NAME
        | ATTR_VOL_MOUNTFLAGS
        | ATTR_VOL_MOUNTEDDEVICE
//      | ATTR_VOL_ENCODINGSUSED
        | ATTR_VOL_CAPABILITIES
        | ATTR_VOL_ATTRIBUTES
//      | ATTR_VOL_INFO
        ;
    attr->f_attributes.validattr.dirattr = 0
        | ATTR_DIR_LINKCOUNT
//      | ATTR_DIR_ENTRYCOUNT
//      | ATTR_DIR_MOUNTSTATUS
        ;
    attr->f_attributes.validattr.fileattr = 0
        | ATTR_FILE_LINKCOUNT
        | ATTR_FILE_TOTALSIZE
        | ATTR_FILE_ALLOCSIZE
        | ATTR_FILE_IOBLOCKSIZE
        | ATTR_FILE_DEVTYPE
//      | ATTR_FILE_FORKCOUNT
//      | ATTR_FILE_FORKLIST
        | ATTR_FILE_DATALENGTH
        | ATTR_FILE_DATAALLOCSIZE
//      | ATTR_FILE_RSRCLENGTH
//      | ATTR_FILE_RSRCALLOCSIZE
        ;

    attr->f_attributes.validattr.forkattr = 0;
//      | ATTR_FORK_TOTALSIZE
//      | ATTR_FORK_ALLOCSIZE
        ;
    
    // All attributes that we do support, we support natively.
    
    attr->f_attributes.nativeattr.commonattr = \
        attr->f_attributes.validattr.commonattr;
    attr->f_attributes.nativeattr.volattr    = \
        attr->f_attributes.validattr.volattr;
    attr->f_attributes.nativeattr.dirattr    = \
        attr->f_attributes.validattr.dirattr;
    attr->f_attributes.nativeattr.fileattr   = \
        attr->f_attributes.validattr.fileattr;
    attr->f_attributes.nativeattr.forkattr   = \
        attr->f_attributes.validattr.forkattr;

    VFSATTR_SET_SUPPORTED(attr, f_attributes);
}

static errno_t
fuse_vfs_getattr(mount_t mp, struct vfs_attr *attr, vfs_context_t context)
{
    int err    = 0;
    int faking = 0;

    struct fuse_dispatcher  fdi;
    struct fuse_statfs_out *fsfo;
    struct fuse_statfs_out  faked;
    struct fuse_data       *data;

    fuse_trace_printf_vfsop();

    data = fuse_get_mpdata(mp);
    if (!data) {
        panic("MacFUSE: no private data for mount point?");
    }

    if (!(data->dataflags & FSESS_INITED)) {
        faking = 1;
        goto dostatfs;
    }

    if ((err = fdisp_simple_vfs_getattr(&fdi, mp, context))) {

         // If we cannot communicate with the daemon (most likely because
         // it's dead), we still want to portray that we are a bonafide
         // file system so that we can be gracefully unmounted.

        if (err == ENOTCONN) {
            faking = 1;
            goto dostatfs;
        }

        return err;
    }

dostatfs:
    if (faking == 1) {
        bzero(&faked, sizeof(faked));
        fsfo = &faked;
    } else {
        fsfo = fdi.answ;
    }

    /* fundamental file system block size; goes into f_bsize */
    fsfo->st.frsize = fuse_round_size(fsfo->st.frsize,
                                      FUSE_MIN_BLOCKSIZE, FUSE_MAX_BLOCKSIZE);

    /* preferred/optimal file system block size; goes into f_iosize */
    fsfo->st.bsize  = fuse_round_size(fsfo->st.bsize,
                                      FUSE_MIN_IOSIZE, FUSE_MAX_IOSIZE);

    /* We must have: f_iosize >= f_bsize */
    if (fsfo->st.bsize < fsfo->st.frsize) {
        fsfo->st.bsize = fsfo->st.frsize;
    }

    /*
     * TBD: Possibility:
     *
     * For actual I/O to MacFUSE's "virtual" storage device, we use
     * data->blocksize and data->iosize. These are really meant to be
     * constant across the lifetime of a single mount. If necessary, we
     * can experiment by updating the mount point's stat with the frsize
     * and bsize values we come across here.
     */

    /*
     * FUSE user daemon will (might) give us this:
     *
     * __u64   blocks;  // total data blocks in the file system
     * __u64   bfree;   // free blocks in the file system
     * __u64   bavail;  // free blocks available to non-superuser
     * __u64   files;   // total file nodes in the file system
     * __u64   ffree;   // free file nodes in the file system
     * __u32   bsize;   // preferred/optimal file system block size
     * __u32   namelen; // maximum length of filenames
     * __u32   frsize;  // fundamental file system block size
     *
     * On Mac OS X, we will map this data to struct vfs_attr as follows:
     *
     *  Mac OS X                     FUSE
     *  --------                     ----
     *  uint64_t f_supported   <-    // handled here
     *  uint64_t f_active      <-    // handled here
     *  uint64_t f_objcount    <-    -
     *  uint64_t f_filecount   <-    files
     *  uint64_t f_dircount    <-    -
     *  uint32_t f_bsize       <-    frsize
     *  size_t   f_iosize      <-    bsize
     *  uint64_t f_blocks      <-    blocks
     *  uint64_t f_bfree       <-    bfree
     *  uint64_t f_bavail      <-    bavail
     *  uint64_t f_bused       <-    blocks - bfree
     *  uint64_t f_files       <-    files
     *  uint64_t f_ffree       <-    ffree
     *  fsid_t   f_fsid        <-    // handled elsewhere
     *  uid_t    f_owner       <-    // handled elsewhere
     *  ... capabilities       <-    // handled here
     *  ... attributes         <-    // handled here
     *  f_create_time          <-    -
     *  f_modify_time          <-    -
     *  f_access_time          <-    -
     *  f_backup_time          <-    -
     *  uint32_t f_fssubtype   <-    // daemon provides
     *  char *f_vol_name       <-    // handled here
     *  uint16_t f_signature   <-    // handled here
     *  uint16_t f_carbon_fsid <-    // handled here
     */

    VFSATTR_RETURN(attr, f_filecount, fsfo->st.files);
    VFSATTR_RETURN(attr, f_bsize, fsfo->st.frsize);
    VFSATTR_RETURN(attr, f_iosize, fsfo->st.bsize);
    VFSATTR_RETURN(attr, f_blocks, fsfo->st.blocks);
    VFSATTR_RETURN(attr, f_bfree, fsfo->st.bfree);
    VFSATTR_RETURN(attr, f_bavail, fsfo->st.bavail);
    VFSATTR_RETURN(attr, f_bused, (fsfo->st.blocks - fsfo->st.bfree));
    VFSATTR_RETURN(attr, f_files, fsfo->st.files);
    VFSATTR_RETURN(attr, f_ffree, fsfo->st.ffree);

    /* f_fsid and f_owner handled elsewhere. */

    /* Handle capabilities and attributes. */
    handle_capabilities_and_attributes(mp, attr);

    VFSATTR_RETURN(attr, f_create_time, kZeroTime);
    VFSATTR_RETURN(attr, f_modify_time, kZeroTime);
    VFSATTR_RETURN(attr, f_access_time, kZeroTime);
    VFSATTR_RETURN(attr, f_backup_time, kZeroTime);

    VFSATTR_RETURN(attr, f_fssubtype, data->fssubtype);

    /* Daemon needs to pass this. */
    if (VFSATTR_IS_ACTIVE(attr, f_vol_name)) {
        if (data->volname[0] != 0) {
            strncpy(attr->f_vol_name, data->volname, MAXPATHLEN);
            attr->f_vol_name[MAXPATHLEN - 1] = 0;
            VFSATTR_SET_SUPPORTED(attr, f_vol_name);
        }
    }

    VFSATTR_RETURN(attr, f_signature, OSSwapBigToHostInt16(FUSEFS_SIGNATURE));
    VFSATTR_RETURN(attr, f_carbon_fsid, 0);

    if (faking == 0) {
        fuse_ticket_drop(fdi.tick);
    }

    return (0);
}

struct fuse_sync_cargs {
    vfs_context_t context;
    int waitfor;
    int error;
};

static int
fuse_sync_callback(vnode_t vp, void *cargs)
{
    int type;
    struct fuse_sync_cargs *args;
    struct fuse_vnode_data *fvdat;
    struct fuse_dispatcher  fdi;
    struct fuse_filehandle *fufh;
    struct fuse_data       *data;
    mount_t mp;

    if (!vnode_hasdirtyblks(vp)) {
        return VNODE_RETURNED;
    }

    mp = vnode_mount(vp);

    if (fuse_isdeadfs_mp(mp)) {
        return VNODE_RETURNED_DONE;
    }

    data = fuse_get_mpdata(mp);

    if (!fuse_implemented(data, (vnode_isdir(vp)) ?
        FSESS_NOIMPLBIT(FSYNCDIR) : FSESS_NOIMPLBIT(FSYNC))) {
        return VNODE_RETURNED;
    }

    args = (struct fuse_sync_cargs *)cargs;
    fvdat = VTOFUD(vp);

    cluster_push(vp, 0);

    fdisp_init(&fdi, 0);
    for (type = 0; type < FUFH_MAXTYPE; type++) {
        fufh = &(fvdat->fufh[type]);
        if (fufh->fufh_flags & FUFH_VALID) {
            fuse_internal_fsync(vp, args->context, fufh, &fdi);
        }
    }

    /*
     * In general:
     *
     * - can use vnode_isinuse() if the need be
     * - vnode and UBC are in lock-step
     * - note that umount will call ubc_sync_range()
     */

    return VNODE_RETURNED;
}

static errno_t
fuse_vfs_sync(mount_t mp, int waitfor, vfs_context_t context)
{
    uint64_t mntflags;
    struct fuse_sync_cargs args;
    int allerror = 0;

    fuse_trace_printf_vfsop();

    mntflags = vfs_flags(mp);

    if (fuse_isdeadfs_mp(mp)) {
        return 0;
    }

    if (vfs_isupdate(mp)) {
        return 0;
    } 

    if (vfs_isrdonly(mp)) {
        return EROFS; // should panic!?
    }

    /*
     * Write back each (modified) fuse node.
     */
    args.context = context;
    args.waitfor = waitfor;
    args.error = 0;

    vnode_iterate(mp, 0, fuse_sync_callback, (void *)&args);

    if (args.error) {
        allerror = args.error;
    }

    /*
     * For other types of stale file system information, such as:
     *
     * - fs control info
     * - quota information
     * - modified superblock
     */

    return allerror;
}

static errno_t
fuse_vfs_setattr(mount_t mp, struct vfs_attr *fsap, vfs_context_t context)
{
    int error = 0;
    struct fuse_data *data;
    kauth_cred_t cred = vfs_context_ucred(context);

    fuse_trace_printf_vfsop();

    if (!fuse_vfs_context_issuser(context) &&
        (kauth_cred_getuid(cred) != vfs_statfs(mp)->f_owner)) {
        return EACCES;
    }

    data = fuse_get_mpdata(mp);

    if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {

        size_t vlen;

        if (!(data->dataflags & FSESS_VOL_RENAME)) {
            error = ENOTSUP;
            goto out;
        }

        if (fsap->f_vol_name[0] == 0) {
            error = EINVAL;
            goto out;
        }

        /*
         * If the FUSE API supported volume name change, we would be sending
         * a message to the FUSE daemon at this point.
         */

        copystr(fsap->f_vol_name, data->volname, MAXPATHLEN - 1, &vlen);
        bzero(data->volname + vlen, MAXPATHLEN - vlen);
        VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
    }

out:
    return error;
}

__private_extern__
int
fuse_setextendedsecurity(mount_t mp, int state)
{
    int err = EINVAL;
    struct fuse_data *data;

    data = fuse_get_mpdata(mp);

    if (!data) {
        return ENXIO;
    }

    if (state == 1) {
        /* Turning on extended security. */
        if ((data->dataflags & FSESS_NO_VNCACHE) ||
            (data->dataflags & FSESS_DEFER_PERMISSIONS)) {
            return EINVAL;
        }
        data->dataflags |= (FSESS_EXTENDED_SECURITY |
                            FSESS_DEFAULT_PERMISSIONS);;
        if (vfs_authopaque(mp)) {
            vfs_clearauthopaque(mp);
        }
        if (vfs_authopaqueaccess(mp)) {
            vfs_clearauthopaqueaccess(mp);
        }
        vfs_setextendedsecurity(mp);
        err = 0;
    } else if (state == 0) {
        /* Turning off extended security. */
        data->dataflags &= ~FSESS_EXTENDED_SECURITY;
        vfs_clearextendedsecurity(mp);
        err = 0;
    }

    return err;
}