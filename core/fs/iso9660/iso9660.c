#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>
#include <core.h>
#include <cache.h>
#include <disk.h>
#include <fs.h>
#include "iso9660_fs.h"

static struct inode *new_iso_inode(struct fs_info *fs)
{
    return alloc_inode(fs, 0, sizeof(uint32_t));
}

static inline struct iso_sb_info * ISO_SB(struct fs_info *fs)
{
    return fs->fs_info;
}

/*
 * Mangle a filename pointed to by src into a buffer pointed
 * to by dst; ends on encountering any whitespace.
 * dst is preserved.
 *
 * This verifies that a filename is < FilENAME_MAX characters,
 * doesn't contain whitespace, zero-pads the output buffer,
 * and removes trailing dots and redumndant slashes, so "repe
 * cmpsb" can do a compare, and the path-searching routine gets
 * a bit of an easier job.
 *
 */
static void iso_mangle_name(char *dst, const char *src)
{
    char *p = dst;
    int i = FILENAME_MAX - 1;

    while (not_whitespace(*src)) {
        if ( *src == '/' ) {
            if ( *(src+1) == '/' ) {
                i--;
                src++;
                continue;
            }
        }

        *dst++ = *src ++;
        i--;
    }

    while ( 1 ) {
        if ( dst == p )
            break;

        if ( (*(dst-1) != '.') && (*(dst-1) != '/') )
            break;

        dst --;
        i ++;
    }

    i ++;
    for (; i > 0; i -- )
        *dst++ = '\0';
}

static int iso_convert_name(char *dst, const char *src, int len)
{
    int i = 0;
    char c;
    
    for (; i < len; i++) {
	c = src[i];
	if (!c)
	    break;
	
	/* remove ';1' in the end */
	if (c == ';' && i == len - 2 && src[i + 1] == '1')
	    break;
	/* convert others ';' to '.' */
	if (c == ';')
	    c = '.';
	*dst++ = c;
    }
    
    /* Then remove the terminal dots */
    while (*(dst - 1) == '.') {
	if (i <= 2)
	    break;
	dst--;
	i--;
    }
    *dst = 0;
    
    return i;
}

/* 
 * Unlike strcmp, it does return 1 on match, or reutrn 0 if not match.
 */
static int iso_compare_name(const char *de_name, int len,
			    const char *file_name)
{
    char iso_file_name[256];
    char *p = iso_file_name;
    char c1, c2;
    int i;
    
    i = iso_convert_name(iso_file_name, de_name, len);
    
    if (i != (int)strlen(file_name))
	return 0;
    
    while (i--) {
	c1 = *p++;
	c2 = *file_name++;
	
	/* convert to lower case */
	c1 |= 0x20;
	c2 |= 0x20;
	if (c1 != c2)
	    return 0;
    }
    
    return 1;
}

static inline int cdrom_read_blocks(struct disk *disk, void *buf, 
				    int block, int blocks)
{
    return disk->rdwr_sectors(disk, buf, block, blocks, 0);
}

/*
 * Get multiple clusters from a file, given the file pointer.
 */
static uint32_t iso_getfssec(struct file *file, char *buf,
			     int blocks, bool *have_more)
{
    struct fs_info *fs = file->fs;
    struct disk *disk = fs->fs_dev->disk;
    uint32_t bytes_read = blocks << fs->block_shift;
    uint32_t bytes_left = file->inode->size - file->offset;
    uint32_t blocks_left = (bytes_left + BLOCK_SIZE(file->fs) - 1) 
	>> file->fs->block_shift;
    block_t block = *(uint32_t *)file->inode->pvt
	+ (file->offset >> fs->block_shift);    

    if (blocks > blocks_left)
        blocks = blocks_left;
    cdrom_read_blocks(disk, buf, block, blocks);

    if (bytes_read >= bytes_left) {
        bytes_read = bytes_left;
        *have_more = 0;
    } else {
        *have_more = 1;
    }
    
    file->offset += bytes_read;
    return bytes_read;
}

/*
 * Find a entry in the specified dir with name _dname_.
 */
static const struct iso_dir_entry *
iso_find_entry(char *dname, struct inode *inode)
{
    struct fs_info *fs = inode->fs;
    block_t dir_block = *(uint32_t *)inode->pvt;
    int i = 0, offset = 0;
    const char *de_name;
    int de_name_len, de_len;
    const struct iso_dir_entry *de;
    struct iso_dir_entry tmpde;
    const char *data = NULL;
    struct cache_struct *cs = NULL;
    
    while (1) {
	if (!data) {
	    if (++i > inode->blocks)
		return NULL;
	    data = get_cache(fs->fs_dev, dir_block++);
	    de = (const struct iso_dir_entry *)data;
	    offset = 0;
	}
	de = (const struct iso_dir_entry *)(data + offset);
	
	de_len = de->length;
	if (de_len == 0) {    /* move on to the next block */
	    cs = NULL;
	    continue;
	}
	offset += de_len;
	
	/* Make sure we have a full directory entry */
	if (offset >= BLOCK_SIZE(fs)) {
	    int slop = de_len + BLOCK_SIZE(fs) - offset;
	    
	    memcpy(&tmpde, de, slop);
	    offset &= BLOCK_SIZE(fs) - 1;
	    if (offset) {
		if (++i > inode->blocks)
		    return NULL;
		data = get_cache(fs->fs_dev, dir_block++);
		memcpy((void *)&tmpde + slop, data, offset);
	    }
	    de = &tmpde;
	}
	
	if (de_len < 33) {
	    printf("Corrupted directory entry in sector %u\n", 
		   (uint32_t)(dir_block - 1));
	    return NULL;
	}
	
	de_name_len = de->name_len;
	de_name = de->name;
	/* Handling the special case ".' and '..' here */
	if((de_name_len == 1) && (*de_name == 0)) {
	    de_name = ".";
	} else if ((de_name_len == 1) && (*de_name == 1)) {
	    de_name ="..";
	    de_name_len = 2;
	}
	if (iso_compare_name(de_name, de_name_len, dname))
	    return de;
    }
}

static inline int get_inode_mode(uint8_t flags)
{
    if (flags & 0x02)
	return I_DIR;
    else
	return I_FILE;
}

static struct inode *iso_get_inode(struct fs_info *fs,
				   const struct iso_dir_entry *de)
{
    struct inode *inode = new_iso_inode(fs);
    if (!inode)
	return NULL;

    inode->mode   = get_inode_mode(de->flags);
    inode->size   = *(uint32_t *)de->size;
    *(uint32_t *)inode->pvt = *(uint32_t *)de->extent;
    inode->blocks = (inode->size + BLOCK_SIZE(fs) - 1) 
	>> fs->block_shift;
    
    return inode;
}


static struct inode *iso_iget_root(struct fs_info *fs)
{
    struct inode *inode = new_iso_inode(fs);
    struct iso_dir_entry *root = &ISO_SB(fs)->root;
    
    if (!inode) 
	return NULL;
    
    inode->mode   = I_DIR;
    inode->size   = *(uint32_t *)root->size;
    *(uint32_t *)inode->pvt = *(uint32_t *)root->extent;
    inode->blocks = (inode->size + BLOCK_SIZE(fs) - 1)
	>> fs->block_shift;
    
    return inode;
}	

static struct inode *iso_iget(char *dname, struct inode *parent)
{
    const struct iso_dir_entry *de;
    
    de = iso_find_entry(dname, parent);
    if (!de)
	return NULL;
    
    return iso_get_inode(parent->fs, de);
}

/* Convert to lower case string */
static void tolower_str(char *str)
{
	while (*str) {
		if (*str >= 'A' && *str <= 'Z')
			*str = *str + 0x20;
		str++;
	}
}

static struct dirent *iso_readdir(struct file *file)
{
    struct fs_info *fs = file->fs;
    struct inode *inode = file->inode;
    const struct iso_dir_entry *de;
    struct iso_dir_entry tmpde;
    struct dirent *dirent;
    const char *data = NULL;
    block_t block =  *(uint32_t *)file->inode->pvt
	+ (file->offset >> fs->block_shift);
    int offset = file->offset & (BLOCK_SIZE(fs) - 1);
    int i = 0;
    int de_len, de_name_len;
    char *de_name;
    
    while (1) {
	if (!data) {
	    if (++i > inode->blocks)
		return NULL;
	    data = get_cache(fs->fs_dev, block++);
	}
	de = (const struct iso_dir_entry *)(data + offset);
	
	de_len = de->length;
	if (de_len == 0) {    /* move on to the next block */
	    data = NULL;
	    file->offset = (file->offset + BLOCK_SIZE(fs) - 1)
		>> fs->block_shift;
	    continue;
	}
	offset += de_len;
	
	/* Make sure we have a full directory entry */
	if (offset >= BLOCK_SIZE(fs)) {
	    int slop = de_len + BLOCK_SIZE(fs) - offset;
	    
	    memcpy(&tmpde, de, slop);
	    offset &= BLOCK_SIZE(fs) - 1;
	    if (offset) {
		if (++i > inode->blocks)
		    return NULL;
		data = get_cache(fs->fs_dev, block++);
		memcpy((char *)&tmpde + slop, data, offset);
	    }
	    de = &tmpde;
	}
	
	if (de_len < 33) {
	    printf("Corrupted directory entry in sector %u\n", 
		   (uint32_t)(block - 1));
	    return NULL;
	}
	
	de_name_len = de->name_len;
	de_name = de->name;
	/* Handling the special case ".' and '..' here */
	if((de_name_len == 1) && (*de_name == 0)) {
	    de_name = ".";
	} else if ((de_name_len == 1) && (*de_name == 1)) {
	    de_name ="..";
	    de_name_len = 2;
	}
	
	break;
    }
    
    if (!(dirent = malloc(sizeof(*dirent)))) {
	malloc_error("dirent structure in iso_readdir");
	return NULL;
    }
    
    dirent->d_ino = 0;           /* Inode number is invalid to ISO fs */
    dirent->d_off = file->offset;
    dirent->d_reclen = de_len;
    dirent->d_type = get_inode_mode(de->flags);
    iso_convert_name(dirent->d_name, de_name, de_name_len);
    tolower_str(dirent->d_name);
    
    file->offset += de_len;  /* Update for next reading */
    
    return dirent;
}

/* Load the config file, return 1 if failed, or 0 */
static int iso_load_config(void)
{
    const char *search_directories[] = {
	"/boot/isolinux", 
	"/isolinux",
	"/",
	NULL
    };
    com32sys_t regs;
    int i;
    
    for (i = 0; search_directories[i]; i++) {
	    memset(&regs, 0, sizeof regs);
	    snprintf(ConfigName, FILENAME_MAX, "%s/isolinux.cfg",
		     search_directories[i]);
	    regs.edi.w[0] = OFFS_WRT(ConfigName, 0);
	    call16(core_open, &regs, &regs);
	    if (!(regs.eflags.l & EFLAGS_ZF))
		break;
    }
    if (!search_directories[i])
	return -1;
    
    /* Set the current working directory */
    chdir(search_directories[i]);
    return 0;
}


static int iso_fs_init(struct fs_info *fs)
{
    struct iso_sb_info *sbi;
    
    sbi = malloc(sizeof(*sbi));
    if (!sbi) {
	malloc_error("iso_sb_info structure");
	return 1;
    }
    fs->fs_info = sbi;
    
    cdrom_read_blocks(fs->fs_dev->disk, trackbuf, 16, 1);
    memcpy(&sbi->root, trackbuf + ROOT_DIR_OFFSET, sizeof(sbi->root));

    fs->sector_shift = fs->fs_dev->disk->sector_shift;
    fs->block_shift  = 11;
    fs->sector_size  = 1 << fs->sector_shift;
    fs->block_size   = 1 << fs->block_shift;

    /* Initialize the cache */
    cache_init(fs->fs_dev, fs->block_shift);

    return fs->block_shift;
}


const struct fs_ops iso_fs_ops = {
    .fs_name       = "iso",
    .fs_flags      = FS_USEMEM | FS_THISIND,
    .fs_init       = iso_fs_init,
    .searchdir     = NULL, 
    .getfssec      = iso_getfssec,
    .close_file    = generic_close_file,
    .mangle_name   = iso_mangle_name,
    .unmangle_name = generic_unmangle_name,
    .load_config   = iso_load_config,
    .iget_root     = iso_iget_root,
    .iget          = iso_iget,
    .readdir       = iso_readdir
};