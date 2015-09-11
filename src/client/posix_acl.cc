#include "include/types.h"
#include <sys/stat.h>
#include "posix_acl.h"

int posix_acl_valid(const void *xattr, size_t size)
{
  const acl_ea_header *header;
  if (size < sizeof(*header))
    return 0;
  header = reinterpret_cast<const acl_ea_header*>(xattr);
  ceph_le32 expected_version;
  expected_version = ACL_EA_VERSION;
  if (header->a_version != expected_version)
    return 0;
  size -= sizeof(*header);
  if (size % sizeof(acl_ea_entry))
    return 0;
  return 1;
}

int posix_acl_equiv_mode(const void *xattr, size_t size, mode_t *mode_p)
{
  if (!posix_acl_valid(xattr, size))
    return -EINVAL;

  int not_equiv = 0;
  mode_t mode = 0;

  const acl_ea_header *header = reinterpret_cast<const acl_ea_header*>(xattr);
  const acl_ea_entry *entry = header->a_entries;
  int count = (size - sizeof(*header)) / sizeof(*entry);
  for (int i = 0; i < count; ++i) {
    __u16 tag = entry->e_tag;
    __u16 perm = entry->e_perm;
    switch(tag) {
      case ACL_USER_OBJ:
	mode |= (perm & S_IRWXO) << 6;
	break;
      case ACL_GROUP_OBJ:
	mode |= (perm & S_IRWXO) << 3;
	break;
      case ACL_OTHER:
	mode |= perm & S_IRWXO;
	break;
      case ACL_MASK:
	mode = (mode & ~S_IRWXG) | ((perm & S_IRWXO) << 3);
	not_equiv = 1;
	break;
      case ACL_USER:
      case ACL_GROUP:
	not_equiv = 1;
	break;
      default:
	return -EINVAL;
    }
    ++entry;
  }
  if (mode_p)
    *mode_p = (*mode_p & ~ACCESSPERMS) | mode;
  return not_equiv;
}

int posix_acl_create_masq(bufferptr& acl, mode_t *mode_p)
{
  if (!posix_acl_valid(acl.c_str(), acl.length()))
    return -EIO;

  acl_ea_entry *group_entry = NULL, *mask_entry = NULL;
  mode_t mode = *mode_p;
  int not_equiv = 0;

  acl_ea_header *header = reinterpret_cast<acl_ea_header*>(acl.c_str());
  acl_ea_entry *entry = header->a_entries;
  int count = (acl.length() - sizeof(*header)) / sizeof(*entry);
  for (int i = 0; i < count; ++i) {
    __u16 tag = entry->e_tag;
    __u16 perm = entry->e_perm;
    switch(tag) {
      case ACL_USER_OBJ:
	perm &= (mode >> 6) | ~S_IRWXO;
	mode &= (perm << 6) | ~S_IRWXU;
	entry->e_perm = perm;
	break;
      case ACL_USER:
      case ACL_GROUP:
	not_equiv = 1;
	break;
      case ACL_GROUP_OBJ:
	group_entry = entry;
	break;
      case ACL_OTHER:
	perm &= mode | ~S_IRWXO;
	mode &= perm | ~S_IRWXO;
	entry->e_perm = perm;
	break;
      case ACL_MASK:
	mask_entry = entry;
	not_equiv = 1;
	break;
      default:
	return -EIO;

    }
    ++entry;
  }

  if (mask_entry) {
    __u16 perm = mask_entry->e_perm;
    perm &= (mode >> 3) | ~S_IRWXO;
    mode &= (perm << 3) | ~S_IRWXG;
    mask_entry->e_perm = perm;
  } else {
    if (!group_entry)
      return -EIO;
    __u16 perm = group_entry->e_perm;
    perm &= (mode >> 3) | ~S_IRWXO;
    mode &= (perm << 3) | ~S_IRWXG;
    group_entry->e_perm = perm;
  }

  *mode_p = (*mode_p & ~ACCESSPERMS) | mode;
  return not_equiv;
}

int posix_acl_chmod_masq(bufferptr& acl, mode_t mode)
{
  if (!posix_acl_valid(acl.c_str(), acl.length()))
    return -EIO;

  acl_ea_entry *group_entry = NULL, *mask_entry = NULL;

  acl_ea_header *header = reinterpret_cast<acl_ea_header*>(acl.c_str());
  acl_ea_entry *entry = header->a_entries;
  int count = (acl.length() - sizeof(*header)) / sizeof(*entry);
  for (int i = 0; i < count; ++i) {
    __u16 tag = entry->e_tag;
    switch(tag) {
      case ACL_USER_OBJ:
	entry->e_perm = (mode & S_IRWXU) >> 6;
	break;
      case ACL_USER:
      case ACL_GROUP:
	break;
      case ACL_GROUP_OBJ:
	group_entry = entry;
	break;
      case ACL_MASK:
	mask_entry = entry;
	break;
      case ACL_OTHER:
	entry->e_perm = mode & S_IRWXO;
	break;
      default:
	return -EIO;
    }
    ++entry;
  }

  if (mask_entry) {
    mask_entry->e_perm = (mode & S_IRWXG) >> 3;
  } else {
    if (!group_entry)
      return -EIO;
    group_entry->e_perm = (mode & S_IRWXG) >> 3;
  }
  return 0;
}

static int in_grouplist(gid_t gid, gid_t *sgids, int sgid_count)
{
  for (int i = 0; i < sgid_count; i++) {
    if (sgids[i] == gid)
      return 1;
  }
  return 0;
}

int posix_acl_permission(const bufferptr& acl, uid_t i_uid, gid_t i_gid,
			 uid_t uid, gid_t gid, gid_t *sgids, int sgid_count,
			 unsigned want)
{
  if (!posix_acl_valid(acl.c_str(), acl.length()))
    return -EIO;

  const acl_ea_header *header = reinterpret_cast<const acl_ea_header*>(acl.c_str());
  const acl_ea_entry *entry = header->a_entries;
  const acl_ea_entry *next_entry;
  __u16 perm, tag;
  __u32 id;
  int found = 0;
  int idx;
  int count = (acl.length() - sizeof(*header)) / sizeof(*entry);
  for (idx = 0; idx < count; ++idx) {
    tag = entry->e_tag;
    perm = entry->e_perm;
    switch(tag) {
      case ACL_USER_OBJ:
	if (i_uid == uid)
	  goto check_perm;
	break;
      case ACL_USER:
	id = entry->e_id;
	if (id == uid)
	  goto mask;
	break;
      case ACL_GROUP_OBJ:
	if (i_gid == gid || in_grouplist(i_gid, sgids, sgid_count)) {
	  found = 1;
	  if ((perm & want) == want)
	    goto mask;
	}
	break;
      case ACL_GROUP:
	id = entry->e_id;
	if (id == gid || in_grouplist(id, sgids, sgid_count)) {
	  found = 1;
	  if ((perm & want) == want)
	    goto mask;
	}
	break;
      case ACL_MASK:
	break;
      case ACL_OTHER:
	if (found)
	  return -EACCES;
	else
	  goto check_perm;
	break;
      default:
	return -EIO;
    }
    ++entry;
  }
  return -EIO;

mask:
  next_entry = entry + 1;
  for (++idx; idx < count; ++idx) {
    tag = next_entry->e_tag;
    if (tag == ACL_MASK) {
      __u16 mask = next_entry->e_perm;
      if ((perm & mask & want) == want)
	return 0;
      return -EACCES;
    }
    ++next_entry;
  }
check_perm:
  if ((perm & want) == want)
    return 0;
  return -EACCES;
}
