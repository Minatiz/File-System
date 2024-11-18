
#ifdef LINUX_SIM
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#endif /* LINUX_SIM */

#include "fs.h"
#include "block.h"
#include "common.h"
#include "fs_error.h"
#include "inode.h"
#include "kernel.h"
#include "superblock.h"
#include "thread.h"
#include "util.h"

#define MAGIC_NUMBER 0x1337C0DE
#define CURRENT_DIR "."
#define PARENT_DIR ".."
#define PATH "/"
#define MAX_FILE_SIZE 4096							  // 4KB
#define FILE_SYS_BYTES ((FS_BLOCKS - 2) * BLOCK_SIZE) // Availble bytes on the image for FS

// OS_size +1 = process directory from kernel.c then we need to go next to find the location in the image where FS starts.
#define SUPER_BLOCK_LOC os_size + 2 // 0
#define BITMAP_LOC os_size + 3		// 1
#define INODE_LOC os_size + 4		// 2
#define DATABLOCK_LOC os_size + 5	// 3

/*
 * The superblock describes the filesystem.
 *
 * The filesystem layout looks like this:
 *    0                    1
 * +-------------+-----------------------------+-/
 * | Super block |  Inode & data bitmap block  |
 * +-------------+-----------------------------+-/
 *  2                         3
 * <------ Inode area ----> <-------- Data block area -------->
 * /--------+-//-+---------+--------------+-//-+--------------+
 *  Inode 1 |    | Inode n | Data block 1 |    | Data block n |
 * /--------+-//-+---------+--------------+-//-+--------------+
 *
 */
#define BITMAP_ENTRIES 256

// Number of inodes in the inode table.
#define INODE_TABLE_ENTRIES 20

// Each keeps track of which block are in use or not.
// The block number of the inode allocation bitmap for this Block Group (inode bitmap block). Inode allocation
static char inode_bmap[BITMAP_ENTRIES];
// The block number of the block allocation bitmap for this Block Group (data bitmap block). Block allocation
static char dblk_bmap[BITMAP_ENTRIES];

static int get_free_entry(unsigned char *bitmap);
static int free_bitmap_entry(int entry, unsigned char *bitmap);
static int add_dir_entry(struct mem_inode *m_inode, char *name, inode_t inode);
static int remove_dir_entry(struct mem_inode *m_inode, char *name);
static int get_fd_entry(void);
int check_dir_empty(inode_t inode);
static int parse_path(char *path, char *argv[MAX_PATH_LEN]);
static inode_t name2inode(char *name);
static inode_t find_dir_entry(inode_t inode, char *name);
static blknum_t ino2blk(inode_t ino);
static blknum_t idx2blk(int index);

struct disk_superblock d_sb;

// Intialize the global inode table.
// Describes the underlying files and multiple global file table entries can refer to same node and to global file table.
struct mem_inode m_inode_table[INODE_TABLE_ENTRIES];

// Lock for file system, file locking
static lock_t fs_lock; //  read() and write() ensures nothing else happens when doing this operation.

void fs_init(void)
{
	block_init();
	lock_init(&fs_lock);
	block_read(SUPER_BLOCK_LOC, (char *)&d_sb);

	// Checking if file system exist by checking magic number. If exist mount the existing file system.
	if (d_sb.magic_number == MAGIC_NUMBER)
	{
		// Mount the existing file system
		// scrprintf(0, 0, "Mounting existing file system\n");
		// scrprintf(3, 0, "Magic number: %d", d_sb.magic_number);
		// scrprintf(4, 0, "Number of blocks: %d", d_sb.ndata_blks);
		// scrprintf(5, 0, "Number of inodes: %d", d_sb.ninodes);

		// Reading the inode table from its location for the file system.
		for (int a = 0; a < INODE_TABLE_ENTRIES; a++)
		{
			block_read_part(INODE_LOC, a, 1, (char *)&m_inode_table[a]);
		}

		// Reading the inode and data block bitmaps from their bitmap location for the file system.
		// Due to this we can see again the inodes and its content after "reboot".
		block_read_part(BITMAP_LOC, 0, BITMAP_ENTRIES, (char *)&inode_bmap);
		block_read_part(BITMAP_LOC, BITMAP_ENTRIES, BITMAP_ENTRIES, (char *)&dblk_bmap);
	}
	// Make file system when it doesn't exist.
	else
	{
		fs_mkfs();
	}
}

void fs_mkfs(void)
{
	// Marking inodes and data blocks as “free”
	// Since in the layout the inode and data share same block. Need to share the block index 1 in FS map.
	for (int a = 0; a < BITMAP_ENTRIES; a++)
	{
		inode_bmap[a] = 0;
		dblk_bmap[a] = 0;
	}

	// Writing the inode and data block bitmaps to their bitmap location for the file system.
	block_modify(BITMAP_LOC, 0, (char *)inode_bmap, BITMAP_ENTRIES);
	block_modify(BITMAP_LOC, BITMAP_ENTRIES, (char *)dblk_bmap, BITMAP_ENTRIES);

	// Formatting the superblock
	inode_t inode_root = get_free_entry((unsigned char *)inode_bmap);		   // Inode for the root directory
	inode_t inode_root_datablock = get_free_entry((unsigned char *)dblk_bmap); // Data block for the root directory
	d_sb.magic_number = MAGIC_NUMBER;
	d_sb.ninodes = INODE_TABLE_ENTRIES;
	d_sb.ndata_blks = DIRENTS_PER_BLK * INODE_NDIRECT;
	d_sb.root_inode = ino2blk(inode_root); // Root directory inode
	d_sb.max_filesize = MAX_FILE_SIZE;

	// Writing the superblock
	block_write(SUPER_BLOCK_LOC, &d_sb);

	// Intializing the global inode table.
	for (int a = 0; a < d_sb.ninodes; a++)
	{
		m_inode_table[a].dirty = '0'; // Not dirty
		m_inode_table[a].inode_num = a;
		m_inode_table[a].open_count = 0;
		m_inode_table[a].pos = 0;
		m_inode_table[a].d_inode.nlinks = 0;
		m_inode_table[a].d_inode.size = 0;
		m_inode_table[a].d_inode.type = 0;
		m_inode_table[a].d_inode.direct[0] = 0;

		// Writing the inode table
		block_modify(ino2blk(a), 0, (char *)&m_inode_table[a].d_inode, sizeof(struct disk_inode));
	}

	// Creating the root directory inode
	struct mem_inode *root_inode = &m_inode_table[inode_root];	   // Root directory inode
	root_inode->d_inode.type = INTYPE_DIR;						   // File type is directory
	root_inode->d_inode.size = 2 * sizeof(struct dirent);		   // For 2 entries in the root directory current and parent.
	root_inode->d_inode.nlinks = 1;								   // Only itself
	root_inode->d_inode.direct[0] = idx2blk(inode_root_datablock); // Allocate a new data block for the file
	root_inode->dirty = '0';									   // Dirty since we modified it.
	root_inode->inode_num = inode_root;							   // Root directory
	root_inode->pos = 0;										   // Position 0
	root_inode->open_count = 0;									   // Open count 0 - Don't wanna delete this

	// Creating the root directory data block
	struct dirent root_dir[DIRENTS_PER_BLK];
	block_read_part(root_inode->d_inode.direct[0], 0, sizeof(struct dirent) * DIRENTS_PER_BLK, (char *)root_dir);

	// Making the root directory entries
	for (uint32_t i = 0; i < DIRENTS_PER_BLK; i++)
	{
		root_dir[i].inode = -1; // -1 means empty
		strcpy(root_dir[i].name, "");
	}
	root_dir[0].inode = inode_root;
	strcpy(root_dir[0].name, CURRENT_DIR); // Current directory
	root_dir[1].inode = inode_root;
	strcpy(root_dir[1].name, PARENT_DIR); // Parent directory

	// Writing the root directory inode
	block_modify(d_sb.root_inode, 0, &root_inode->d_inode, sizeof(struct disk_inode));

	// Writing the root directory data block
	block_modify(root_inode->d_inode.direct[0], 0, (char *)root_dir, sizeof(struct dirent) * DIRENTS_PER_BLK);

	// Initializing file descriptor table for process. Each process has a file descriptor table, access via OS only!
	for (int a = 0; a < MAX_OPEN_FILES; a++)
	{
		// Flag set as unused. changes at fs_open.
		current_running->filedes[a].mode = MODE_UNUSED;

		// Set as non negative integer when used.
		current_running->filedes[a].idx = -1;
	}
}

// Finds a free entry in file descriptor table and returns it. If no free entry return error.
int get_fd_entry()
{
	for (int a = 0; a < MAX_OPEN_FILES; a++)
	{
		if (current_running->filedes[a].mode == MODE_UNUSED)
			return a;
	}

	return FSE_NOMOREFDTE;
}

// Adds dir entry to a file or directory
int add_dir_entry(struct mem_inode *m_inode, char *name, inode_t inode)
{
	// Making directory entries
	struct dirent dir[DIRENTS_PER_BLK];
	block_read_part(m_inode->d_inode.direct[0], 0, sizeof(struct dirent) * DIRENTS_PER_BLK, (char *)dir);

	// Find the first free entry to add
	int use_entry = -1;
	for (uint32_t i = 0; i < DIRENTS_PER_BLK; i++)
	{
		if (dir[i].inode == -1)
		{

			use_entry = i;
			break;
		}
	}

	// If found use entry continue
	if (use_entry != -1)
	{
		// Add the new entry
		dir[use_entry].inode = inode;
		strcpy(dir[use_entry].name, name);

		// Write the data block for the directory
		block_modify(m_inode->d_inode.direct[0], 0, (char *)dir, sizeof(struct dirent) * DIRENTS_PER_BLK);

		// Update the inode
		m_inode->d_inode.size += sizeof(struct dirent);

		// Write the inode
		block_modify(ino2blk(m_inode->inode_num), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

		return FSE_OK;
	}

	// Directory entry not found.
	return FSE_DENOTFOUND;
}

// Removes dir entry from a file or directory.
int remove_dir_entry(struct mem_inode *m_inode, char *name)
{
	// Making directory entries
	struct dirent dir[DIRENTS_PER_BLK];
	// Since we already made nr of entries in our add dir function now we want to find the number of entries made.
	int num_entries = m_inode->d_inode.size / sizeof(struct dirent);

	// Read the entire directory array from block
	block_read_part(m_inode->d_inode.direct[0], 0, m_inode->d_inode.size, (char *)dir);

	// Find the first free entry to remove
	int remove_entry = -1;
	for (int i = 0; i < num_entries; i++)
	{
		if (same_string(dir[i].name, name))
		{
			remove_entry = i;
			break;
		}
	}

	// If found remove entry continue
	if (remove_entry != -1)
	{

		// Shifting the element to left when we remove the entry, so that we don't have any empty space in the middle or "garbage".
		// Fixes various bugs that can occur when we remove elements in the middle or near head/tail.
		while (remove_entry < (num_entries - 1))
		{
			dir[remove_entry] = dir[remove_entry + 1];
			remove_entry++;
		}

		// Removes the entry
		dir[num_entries - 1].inode = -1;	   // Setting it to -1 means empty entry and can be reused.
		strcpy(dir[num_entries - 1].name, ""); // and the name to empty.

		// Writing the entry to block.
		block_modify(m_inode->d_inode.direct[0], 0, (char *)dir, m_inode->d_inode.size);

		// Update the inode size
		m_inode->d_inode.size -= sizeof(struct dirent);

		// Writing the entry to inode.
		block_modify(ino2blk(m_inode->inode_num), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

		return FSE_OK;
	}

	// Directory entry not found.
	return FSE_DENOTFOUND;
}

// Open existing file, create a new file if it does not exist
int fs_open(const char *filename, int mode)
{

	int fd = get_fd_entry(); // Get a free file descriptor entry

	int index_inode = name2inode((char *)filename); // Get the inode number of the file
	// scrprintf(7, 0, "index_inode:%d, filename:%s, mode:%d \n\n", index_inode, filename, mode);

	// If file exist. Open the file.
	if ((mode == MODE_RDONLY) && (index_inode != -1))
	{
		// Reading a file that doesnt exist.
		if (index_inode == -1)
			return FSE_NOTEXIST;

		// Get the inode entry
		struct mem_inode *m_inode = &m_inode_table[index_inode];

		// read the inode entry
		block_read_part(ino2blk(index_inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

		// Update the file descriptor entry and global table
		current_running->filedes[fd].mode = mode;
		current_running->filedes[fd].idx = index_inode;
		m_inode_table[index_inode].pos = 0;
		m_inode_table[index_inode].open_count = 1;
		m_inode_table[index_inode].inode_num = index_inode;

		return fd;
	}

	if ((mode & (MODE_WRONLY | MODE_CREAT | MODE_TRUNC)) && (index_inode == -1))
	{

		// check if filename is too long
		if (strlen(filename) > MAX_FILENAME_LEN)
			return FSE_NAMETOLONG;

		// Get a free inode bmap entry
		int free_inode = get_free_entry((unsigned char *)inode_bmap);
		if (free_inode > BITMAP_ENTRIES || free_inode < 0)
			return FSE_BITMAP;

		// Used up all inodes.
		if (free_inode >= INODE_TABLE_ENTRIES)
		{
			return FSE_NOMOREINODES;
		}

		// Geta free data block bmap entry
		int free_dblk = get_free_entry((unsigned char *)dblk_bmap);
		if (free_dblk > BITMAP_ENTRIES || free_dblk < 0)
			return FSE_BITMAP;

		// Update the bitmap
		block_modify(BITMAP_LOC, 0, (char *)inode_bmap, BITMAP_ENTRIES);
		block_modify(BITMAP_LOC, BITMAP_ENTRIES, (char *)dblk_bmap, BITMAP_ENTRIES);

		// Get the inode entry
		struct mem_inode *m_inode = &m_inode_table[free_inode];
		block_read_part(ino2blk(free_inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

		// Updating the inode entries
		m_inode->d_inode.type = INTYPE_FILE;
		m_inode->d_inode.size = 0;
		m_inode->d_inode.nlinks = 1;
		m_inode->d_inode.direct[0] = idx2blk(free_dblk);
		m_inode->dirty = '1';
		m_inode->inode_num = free_inode;
		m_inode->open_count = 1;
		m_inode->pos = 0;

		// Update the file descriptor entry and global table
		current_running->filedes[fd].mode = mode;
		current_running->filedes[fd].idx = free_inode;
		m_inode_table[free_inode].pos = 0;
		m_inode_table[free_inode].open_count = 1;
		m_inode_table[free_inode].inode_num = free_inode;

		// Add the file to the current working directory
		struct mem_inode *cwd_inode = &m_inode_table[current_running->cwd];
		block_read_part(ino2blk(cwd_inode->inode_num), 0, sizeof(struct disk_inode), &cwd_inode->d_inode);
		add_dir_entry(cwd_inode, (char *)filename, free_inode);

		// write the data block entry
		block_modify((idx2blk(free_dblk)), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

		// Write the inode entry
		block_modify((ino2blk(free_inode)), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

		return fd;
	}

	// If read only and doesnt exist, file doesnt exist.
	if (mode == MODE_RDONLY && index_inode == -1)
	{

		return FSE_NOTEXIST;
	}

	return FSE_EXIST; // File already exist, exhausted all the cases.
}

int fs_close(int fd)
{

	// acquire lock
	lock_acquire(&fs_lock);

	// Getting the file descriptor entry for the inode
	struct fd_entry fd_entry = current_running->filedes[fd];
	struct mem_inode *m_inode = &m_inode_table[fd_entry.idx];

	block_read_part(ino2blk(m_inode->inode_num), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// If the file descriptor is not open
	if (fd_entry.mode == MODE_UNUSED)
		return FSE_FDER;

	// If the inode is dirty we need to write it to the disk.
	if (m_inode->dirty == '1')
		block_modify(ino2blk(m_inode->inode_num), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

	// Updating the file descriptor entries, resetting them.
	fd_entry.mode = MODE_UNUSED;
	fd_entry.idx = -1;
	m_inode->open_count = 0;

	current_running->filedes[fd] = fd_entry; // Updating the file descriptor entry

	// release lock
	lock_release(&fs_lock);
	return FSE_OK;
}

// Helper function to calculate block number, offset, and read size. Used in fs_read and fs_write. Is for the data block.
// following a bit the logic from createimage.c
void calculate_block(struct mem_inode *m_inode, int *block_num, int *block_offset, int *read_size, int size)
{
	// Calculating the block number based of the position.
	*block_num = m_inode->pos / (MAX_FILE_SIZE / INODE_NDIRECT);

	// Calculating the offset of the block based of the position
	*block_offset = m_inode->pos % (MAX_FILE_SIZE / INODE_NDIRECT);

	// Calculating the read/write size. If the size is less then the remaining block space then we read/write the size. Else we read/write the remaining block space.
	int remaining_block_space = (MAX_FILE_SIZE / INODE_NDIRECT) - *block_offset;
	*read_size = (size < remaining_block_space) ? size : remaining_block_space;
}

int fs_read(int fd, char *buffer, int size)
{
	// acquire lock
	lock_acquire(&fs_lock);

	if (fd < 0 || fd >= MAX_OPEN_FILES)
		return FSE_FDER;

	// Getting the file descriptor entry for the inode
	struct fd_entry fd_entry = current_running->filedes[fd];
	struct mem_inode m_inode = m_inode_table[fd_entry.idx];

	// Reading the inode from the disk.
	block_read_part(ino2blk(m_inode.inode_num), 0, sizeof(struct disk_inode), &m_inode.d_inode);

	// Temporary buffer for data block
	char block[MAX_FILE_SIZE / INODE_NDIRECT];

	int bytes_to_read = 0, block_num = 0, block_offset = 0, read_size = 0;

	// If the position of the inode is less then the size of inode and at same time the bytes to read is less then the size.
	// not end of the file and need to read more.
	if ((m_inode.pos < m_inode.d_inode.size) && (bytes_to_read < size))
	{
		// Calculating the block number, offset, and read size.
		calculate_block(&m_inode, &block_num, &block_offset, &read_size, size);

		// Using the block num to get the correct data block.
		int block_address = m_inode.d_inode.direct[block_num];

		// Reading the data block
		block_read_part(block_address, block_offset, read_size, block);

		// Copying the read size from the temporary buffer to the buffer passed in.
		bcopy(block, buffer, read_size);

		// Writing the data block
		block_modify(block_address, block_offset, block, read_size);

		// Incrementing the bytes to read.
		bytes_to_read += read_size;
	}

	// Incrementing the position of the inode.
	m_inode.pos += bytes_to_read;

	// Updating the inode table.
	m_inode_table[m_inode.inode_num] = m_inode;

	// Writing to disk to update the inode.
	block_modify(ino2blk(m_inode.inode_num), 0, (char *)&m_inode.d_inode, sizeof(struct disk_inode));

	// release lock
	lock_release(&fs_lock);

	// printf("\t\tbytes_to_read: %d, read_size: %d,  m_inode.pos: %d, m_inode.d_inode.size: %d, block_num: %d, block_offset: %d\n", bytes_to_read, read_size, m_inode.pos, m_inode.d_inode.size, block_num, block_offset);
	return bytes_to_read;
}

int fs_write(int fd, char *buffer, int size)
{
	// acquire lock
	lock_acquire(&fs_lock);

	if (fd < 0 || fd >= MAX_OPEN_FILES)
		return FSE_INVALIDHANDLE;

	if (current_running->filedes[fd].mode == MODE_RDONLY)
		return FSE_INVALIDMODE;

	// Indicates the end of the file so we can stop writing.
	if (size == 0)
		return FSE_OK;

	// Getting the file descriptor entry for the inode
	struct fd_entry fd_entry = current_running->filedes[fd];
	struct mem_inode m_inode = m_inode_table[fd_entry.idx];

	// Reading the inode from the disk.
	block_read_part(ino2blk(m_inode.inode_num), 0, sizeof(struct disk_inode), &m_inode.d_inode);

	// Temporary buffer for data block
	char block[MAX_FILE_SIZE / INODE_NDIRECT];

	int bytes_to_write = 0, block_num = 0, block_offset = 0, write_size = 0;

	// Keep writing until we reach max file size and have writting everything.
	if ((m_inode.pos < MAX_FILE_SIZE) && (bytes_to_write < size))
	{
		// Calculating the block number, offset, and read size.
		calculate_block(&m_inode, &block_num, &block_offset, &write_size, size);

		// Using the block num to get the correct data block.
		int block_address = m_inode.d_inode.direct[block_num];

		// Reading the data block
		block_read_part(block_address, block_offset, write_size, block);

		// Allocates data block for the next data block. Since each data block stores 512 bytes and if we write more then 512 bytes we need to allocate a new data block.
		if (block_address == 0)
		{
			// Getting free data block entries
			int free_dblk = get_free_entry((unsigned char *)dblk_bmap);
			if (free_dblk > BITMAP_ENTRIES || free_dblk < 0)
				return FSE_BITMAP;

			// Update the bitmap
			block_modify(BITMAP_LOC, BITMAP_ENTRIES, (char *)dblk_bmap, BITMAP_ENTRIES);

			m_inode.d_inode.direct[block_num] = idx2blk(free_dblk); // Update the inode
			block_address = m_inode.d_inode.direct[block_num];		// Update the block address to the new block

			// Write to disk
			block_modify(ino2blk(m_inode.inode_num), 0, (char *)&m_inode.d_inode, sizeof(struct disk_inode));

			scrprintf(6, 0, "\t## Allocated new block:%d ##\n", block_num);
		}

		// Copying the write size from the the buffer and send to the temporary buffer.
		bcopy(buffer, block, write_size);

		// Writing the data block
		block_modify(block_address, block_offset, block, write_size);

		// Incrementing the bytes to write.
		bytes_to_write += write_size;
	}
	// Incrementing the position of the inode.
	m_inode.pos += bytes_to_write;

	// Updating the size of the inode, since we have written to the file.
	m_inode.d_inode.size += bytes_to_write;

	// Updating the inode table.
	m_inode_table[m_inode.inode_num] = m_inode;

	// Writing to disk to update the inode.
	block_modify(ino2blk(m_inode.inode_num), 0, (char *)&m_inode.d_inode, sizeof(struct disk_inode));

	// release lock
	lock_release(&fs_lock);

	// printf("\t\tbytes_to_write: %d, write_size: %d,  m_inode.pos: %d, m_inode.d_inode.size: %d, block_num: %d, block_offset: %d\n", bytes_to_write, write_size, m_inode.pos, m_inode.d_inode.size, block_num, block_offset);

	return bytes_to_write;
}

/*
 * fs_lseek:
 * This function is really incorrectly named, since neither its offset
 * argument or its return value are longs (or off_t's).
 */
int fs_lseek(int fd, int offset, int whence)
{

	// Finding inode entry from file descriptor entry
	struct fd_entry fd_entry = current_running->filedes[fd];
	struct mem_inode *m_inode = &m_inode_table[fd_entry.idx];
	block_read_part(ino2blk(m_inode->inode_num), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// fd is not a open file descriptor similar to EBADF
	if (fd_entry.mode == MODE_UNUSED)
		return FSE_INVALIDMODE;

	// Checking for valid offset, no negative numbers
	if (offset < 0)
		return FSE_INVALIDOFFSET;

	// Checking for valid whence
	if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
		return FSE_ERROR;

	// Following same logic as in Linux man page for lseek.
	// SEEK_SET
	// The file offset is set to offset bytes.

	// 	SEEK_CUR
	// 		The file offset is set to its current location plus offset bytes.

	// 	SEEK_END
	// 		The file offset is set to the size of the file plus offset bytes.
	if (whence == SEEK_SET)
		m_inode->pos = offset;
	else if (whence == SEEK_CUR)
		m_inode->pos += offset;
	else if (whence == SEEK_END)
		m_inode->pos = m_inode->d_inode.size + offset;

	// Write the inode entry
	block_modify(ino2blk(m_inode->inode_num), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

	return m_inode->pos;
}

int fs_mkdir(char *dirname)
{

	// Only to check if directory exist
	int index_inode = name2inode((char *)dirname); // Get the inode number of the file

	// Check if the filename length is below max
	if (strlen(dirname) > MAX_FILENAME_LEN)
		return FSE_NAMETOLONG;

	// Check if directory or filename exist
	if (index_inode != -1) // If directory or filename exist return error
		return FSE_EXIST;

	// Get a free inode
	int free_inode = get_free_entry((unsigned char *)inode_bmap);
	if (free_inode > BITMAP_ENTRIES || free_inode < 0)
		return FSE_BITMAP;

	// Used up all inodes.
	if (free_inode >= INODE_TABLE_ENTRIES)
	{
		return FSE_NOMOREINODES;
	}

	// Get a free data block
	int free_dblk = get_free_entry((unsigned char *)dblk_bmap);
	if (free_dblk > BITMAP_ENTRIES || free_dblk < 0)
		return FSE_BITMAP;

	// Update the bitmap
	block_modify(BITMAP_LOC, 0, (char *)inode_bmap, BITMAP_ENTRIES);
	block_modify(BITMAP_LOC, BITMAP_ENTRIES, (char *)dblk_bmap, BITMAP_ENTRIES);

	// Get the inode entry
	struct mem_inode *m_inode = &m_inode_table[free_inode];
	block_read_part(ino2blk(free_inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// Update the inode entry
	m_inode->d_inode.type = INTYPE_DIR;
	m_inode->d_inode.size = 2 * sizeof(struct dirent);
	m_inode->d_inode.nlinks++;
	m_inode->d_inode.direct[0] = idx2blk(free_dblk);
	m_inode->dirty = '1';
	m_inode->inode_num = free_inode;
	m_inode->open_count = 1;
	m_inode->pos = 0;

	// Updating inode table with new information about the inode.
	m_inode_table[free_inode] = *m_inode;

	// Create the directory data block
	struct dirent dir[DIRENTS_PER_BLK];

	// Intializing the directory entries as empty in the new directory.
	// If not doing this then the directory entries will have garbage values. and funny stuff happens when making directories in depths.
	for (uint32_t i = 0; i < DIRENTS_PER_BLK; i++)
	{
		dir[i].inode = -1;
		strcpy(dir[i].name, "");
	}

	// Add the directory to the current working directory
	// Its from here we add sub directory
	struct mem_inode *cwd_inode = &m_inode_table[current_running->cwd];
	block_read_part(ino2blk(cwd_inode->inode_num), 0, sizeof(struct disk_inode), &cwd_inode->d_inode);
	add_dir_entry(cwd_inode, (char *)dirname, free_inode);

	// Adding the current and parent directory entries.
	dir[0].inode = free_inode;
	strcpy(dir[0].name, CURRENT_DIR);
	dir[1].inode = cwd_inode->inode_num;
	strcpy(dir[1].name, PARENT_DIR);

	// Write the inode entry
	block_modify((ino2blk(free_inode)), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

	// Write the directory data blockd
	block_modify(m_inode->d_inode.direct[0], 0, (char *)dir, sizeof(struct dirent) * DIRENTS_PER_BLK);

	return FSE_OK;
}

int fs_chdir(char *path)
{
	// Get the inode number of the directory
	int index_inode = name2inode(path);

	// If the file does not exist return error.
	if (index_inode == -1)
		return FSE_NOTEXIST;

	// Get the inode entry
	struct mem_inode *m_inode = &m_inode_table[index_inode];
	block_read_part(ino2blk(index_inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// Checking if the inode is a directory
	if (m_inode->d_inode.type != INTYPE_DIR)
		return FSE_DIRISFILE;

	// Updating the current working directory to point to index inode this allows us to cd into the directory.
	// Fixes also the problem when we exit the shell and reenter we can cd around in the existing directories.
	current_running->cwd = index_inode;

	// Write the inode entry
	block_modify(ino2blk(index_inode), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

	return FSE_OK;
}

int fs_rmdir(char *path)
{
	// Dont delete roots, current or parent entries. Somehow path can be removed and messes up the root.
	if (same_string(path, CURRENT_DIR) || same_string(path, PARENT_DIR) || same_string(path, PATH))
		return FSE_INVALIDMODE;

	// Get the inode number of the directory
	int index_inode = name2inode(path);

	// If the directory does not exist return error
	if (index_inode == -1)
		return FSE_NOTEXIST;

	// check if the directory is empty, dont want to delete a directory that is not empty.
	if (check_dir_empty(index_inode) == 0)
		return FSE_DIRNOTEMPTY;

	// Get the inode entry
	struct mem_inode *m_inode = &m_inode_table[index_inode];
	block_read_part(ino2blk(index_inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// Check if the inode is a file, only directories can be removed here
	if (m_inode->d_inode.type == INTYPE_FILE)
		return FSE_DIRISFILE;

	// Remove the file from the current working directory
	struct mem_inode *cwd_inode = &m_inode_table[current_running->cwd];
	block_read_part(ino2blk(cwd_inode->inode_num), 0, sizeof(struct disk_inode), &cwd_inode->d_inode);
	remove_dir_entry(cwd_inode, (char *)path);

	// write datablock entry
	block_modify(m_inode->d_inode.direct[0], 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

	// Write the inode entry
	block_modify(ino2blk(index_inode), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

	// free bitmaps for the inode and data block for directory
	free_bitmap_entry(m_inode->inode_num, (unsigned char *)inode_bmap);
	free_bitmap_entry(m_inode->d_inode.direct[0], (unsigned char *)dblk_bmap);

	// write the bitmaps to disk
	block_modify(BITMAP_LOC, 0, (char *)inode_bmap, BITMAP_ENTRIES);
	block_modify(BITMAP_LOC, BITMAP_ENTRIES, (char *)dblk_bmap, BITMAP_ENTRIES);

	return FSE_OK;
}

int fs_link(char *linkname, char *filename)
{

	// If the linkname already exist return error
	if (name2inode(linkname) != -1)
		return FSE_EXIST;

	// Check if the filename length is below max
	if (strlen(linkname) > MAX_FILENAME_LEN)
		return FSE_NAMETOLONG;

	// Get the inode number of the file
	int index_inode = name2inode(filename);

	// If the file does not exist return error
	if (index_inode == -1)
		return FSE_NOTEXIST;

	// Get the inode entry
	struct mem_inode *m_inode = &m_inode_table[index_inode];
	block_read_part(ino2blk(index_inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// Checking if the inode is a directory, since only files can be hard linked.
	if (m_inode->d_inode.type == INTYPE_DIR)
		return FSE_FILEISDIR;

	// Increasing link count since we linked it
	m_inode->d_inode.nlinks++;

	// Add the file to the current working directory
	struct mem_inode *cwd_inode = &m_inode_table[current_running->cwd];
	block_read_part(ino2blk(cwd_inode->inode_num), 0, sizeof(struct disk_inode), &cwd_inode->d_inode);
	add_dir_entry(cwd_inode, (char *)linkname, index_inode);

	// Write the inode entry
	block_modify(ino2blk(index_inode), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

	return FSE_OK;
}

int fs_unlink(char *linkname)
{
	// Don't delete root current or parent directory
	if (same_string(linkname, CURRENT_DIR) || same_string(linkname, PARENT_DIR))
	{
		return FSE_INVALIDMODE;
	}

	// Get the inode number of the file
	int index_inode = name2inode(linkname);

	// If the file does not exist return error
	if (index_inode == -1)
		return FSE_NOTEXIST;

	// read the inode entry
	struct mem_inode *m_inode = &m_inode_table[index_inode];
	block_read_part(ino2blk(index_inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// Check if the inode is a directory, only files can be removed here
	if (m_inode->d_inode.type == INTYPE_DIR)
		return FSE_FILEISDIR;

	// Decreasing link count since we unlinked it.
	m_inode->d_inode.nlinks--;

	// Delete when nlinks is 1 or less.
	if (m_inode->d_inode.nlinks <= 1)
	{

		// Remove the file from the current working directory
		struct mem_inode *cwd_inode = &m_inode_table[current_running->cwd];
		block_read_part(ino2blk(cwd_inode->inode_num), 0, sizeof(struct disk_inode), &cwd_inode->d_inode);
		remove_dir_entry(cwd_inode, (char *)linkname);

		// Write the inode entry
		block_modify(ino2blk(index_inode), 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));

		// When the orginal file we linked to is removed we write it to disk.
		// This is due when we remove the hardlink we dont remove the content in the orginale file
		if (m_inode->d_inode.nlinks <= 0)
		{
			block_modify(m_inode->d_inode.direct[0], 0, (char *)&m_inode->d_inode, sizeof(struct disk_inode));
		}

		// free the inode and data block for the file.
		free_bitmap_entry(m_inode->inode_num, (unsigned char *)inode_bmap);

		free_bitmap_entry(m_inode->d_inode.direct[0], (unsigned char *)dblk_bmap);

		// write the bitmaps to disk
		block_modify(BITMAP_LOC, 0, (char *)inode_bmap, BITMAP_ENTRIES);
		block_modify(BITMAP_LOC, BITMAP_ENTRIES, (char *)dblk_bmap, BITMAP_ENTRIES);

		return FSE_OK;
	}

	return FSE_OK;
}

int fs_stat(int fd, char *buffer)
{

	// Finding file descriptor entry
	struct fd_entry fd_entry = current_running->filedes[fd];

	// Finding inode entry from file descriptor idx
	struct mem_inode *m_inode = &m_inode_table[fd_entry.idx];

	// if file doesnt exist return error
	if (m_inode->inode_num == -1)
		return FSE_NOTEXIST;

	// Read the inode entry
	block_read_part(ino2blk(m_inode->inode_num), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// Sending the inode stats to the buffer, so when shell_sim/shell calls on stat will get the inode stats.
	buffer[0] = m_inode->d_inode.type;									// 0 = file, 1 = dir
	buffer[1] = m_inode->d_inode.nlinks;								// number of links
	buffer[2] = m_inode->d_inode.direct[0];								// data block number, the number of the start of the data block location.
	bcopy((char *)&m_inode->d_inode.size, &buffer[3], sizeof(inode_t)); // size of the file

	return FSE_OK;
}

/*
 * Helper functions for the system calls
 */

/*
 * get_free_entry:
 *
 * Search the given bitmap for the first zero bit.  If an entry is
 * found it is set to one and the entry number is returned.  Returns
 * -1 if all entrys in the bitmap are set.
 */
static int get_free_entry(unsigned char *bitmap)
{
	int i;

	/* Seach for a free entry */
	for (i = 0; i < BITMAP_ENTRIES / 8; i++)
	{
		if (bitmap[i] == 0xff) /* All taken */
			continue;
		if ((bitmap[i] & 0x80) == 0)
		{ /* msb */
			bitmap[i] |= 0x80;
			return i * 8;
		}
		else if ((bitmap[i] & 0x40) == 0)
		{
			bitmap[i] |= 0x40;
			return i * 8 + 1;
		}
		else if ((bitmap[i] & 0x20) == 0)
		{
			bitmap[i] |= 0x20;
			return i * 8 + 2;
		}
		else if ((bitmap[i] & 0x10) == 0)
		{
			bitmap[i] |= 0x10;
			return i * 8 + 3;
		}
		else if ((bitmap[i] & 0x08) == 0)
		{
			bitmap[i] |= 0x08;
			return i * 8 + 4;
		}
		else if ((bitmap[i] & 0x04) == 0)
		{
			bitmap[i] |= 0x04;
			return i * 8 + 5;
		}
		else if ((bitmap[i] & 0x02) == 0)
		{
			bitmap[i] |= 0x02;
			return i * 8 + 6;
		}
		else if ((bitmap[i] & 0x01) == 0)
		{ /* lsb */
			bitmap[i] |= 0x01;
			return i * 8 + 7;
		}
	}
	return -1;
}

/*
 * free_bitmap_entry:
 *
 * Free a bitmap entry, if the entry is not found -1 is returned, otherwise zero.
 * Note that this function does not check if the bitmap entry was used (freeing
 * an unused entry has no effect).
 */
static int free_bitmap_entry(int entry, unsigned char *bitmap)
{
	unsigned char *bme;

	if (entry >= BITMAP_ENTRIES)
		return -1;

	bme = &bitmap[entry / 8];

	switch (entry % 8)
	{
	case 0:
		*bme &= ~0x80;
		break;
	case 1:
		*bme &= ~0x40;
		break;
	case 2:
		*bme &= ~0x20;
		break;
	case 3:
		*bme &= ~0x10;
		break;
	case 4:
		*bme &= ~0x08;
		break;
	case 5:
		*bme &= ~0x04;
		break;
	case 6:
		*bme &= ~0x02;
		break;
	case 7:
		*bme &= ~0x01;
		break;
	}

	return 0;
}

/*
 * ino2blk:
 * Returns the filesystem block (block number relative to the super
 * block) corresponding to the inode number passed.
 */
static blknum_t ino2blk(inode_t ino)
{

	return (blknum_t)ino + (INODE_LOC);
}

/*
 * idx2blk:
 * Returns the filesystem block (block number relative to the super
 * block) corresponding to the data block index passed.
 */
static blknum_t idx2blk(int index)
{
	return (blknum_t)index + (INODE_LOC + INODE_TABLE_ENTRIES);
}

/*
 * name2inode:
 * Parses a file name and returns the corresponding inode number. If
 * the file cannot be found, -1 is returned.
 */
static inode_t name2inode(char *name)
{
	struct dirent dir[DIRENTS_PER_BLK]; // Directory entries
	// Pointer to the path names.
	char *paths[MAX_PATH_LEN + 1];
	int path_length;
	int name_length = strlen(name);

	// Path name it self. Stores the name.
	char path_name[MAX_PATH_LEN + 1];

	// Copying the name to path_name so that we can modify it.
	bcopy(name, path_name, name_length);
	struct mem_inode *m_inode;
	inode_t inode;

	// If the name is empty.
	if (name_length == 0)
		return FSE_INVALIDNAME;

	// If the name is too long.
	if (name_length > MAX_PATH_LEN)
		return FSE_NAMETOLONG;

	// Null terminating string
	path_name[name_length] = '\0';

	// Resetting name pointer to point to the beginning of the string.
	name = path_name;

	// If it is root directory, return root directory inode number which is 0
	if (same_string(name, PATH))
		return 0;

	// If "/" the path is absolute
	if (name[0] == '/')
	{
		// scrprintf(0, 0, "hit2\n");
		// Absolute path starts at root directory
		m_inode = &m_inode_table[0];
		// Read the root directory inode
		block_read_part(ino2blk(m_inode->inode_num), 0, sizeof(struct disk_inode), &m_inode->d_inode);
		name++;
		name_length--;
	}
	// Else it's relative path
	else
	{
		// scrprintf(0, 0, "hit3\n");
		// Relative path starts at cwd
		m_inode = &m_inode_table[current_running->cwd];
		// Read the cwd inode
		block_read_part(ino2blk(m_inode->inode_num), 0, sizeof(struct disk_inode), &m_inode->d_inode);
	}

	// Parses the path and return number of length of the path.
	path_length = parse_path(name, paths);
	if (path_length <= 0)
		return FSE_PARSEERROR;

	// Loop through the directory entries to find the file and return the inode number in the end.
	for (int i = 0; i < path_length; i++)
	{
		inode = find_dir_entry(m_inode->inode_num, paths[i]);
		// Entry found we read it and update m_inode table
		if (inode != -1)
		{
			m_inode = &m_inode_table[inode];
			block_read_part(ino2blk(m_inode->inode_num), 0, sizeof(struct disk_inode), &m_inode->d_inode);
		}
		// If the entry is not found, means we found no files or directories in the path. Return error. Sanity check.
		else
			return FSE_ERROR;
	}

	return inode; // return the inode number of the file
}

// check if the directory on different path has files or directories in it, so we dont delete a directory that is not empty and has files in it
int check_dir_empty(inode_t inode)
{
	// Read the inode table entry
	struct mem_inode *m_inode = &m_inode_table[inode];
	block_read_part(ino2blk(inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	// Check if the directory is empty, if its above 40 bytes there exist more files or directories in it
	if ((uintptr_t)m_inode->d_inode.size > (2 * sizeof(struct dirent)))
		return 0;

	return 1;
}

// Taken from shell.c
/* Extract every directory name out of a path. This consist of replacing every /
 * with '\0' */
static int parse_path(char *path, char *argv[MAX_PATH_LEN])
{
	char *s = path;
	int argc = 0;

	argv[argc++] = path;

	while (*s != '\0')
	{
		if (*s == '/')
		{
			*s = '\0';
			argv[argc++] = (s + 1);
		}

		s++;
	}

	return argc;
}

// Finds directory entries and returns the inode number, by checking current and parent directories for the name and adding them to list.
static inode_t find_dir_entry(inode_t inode, char *name)
{
	// Reading the inode table entry
	struct mem_inode *m_inode = &m_inode_table[inode];
	block_read_part(ino2blk(inode), 0, sizeof(struct disk_inode), &m_inode->d_inode);

	struct dirent dir[DIRENTS_PER_BLK];
	// Reading the directory entries in the data block
	block_read_part(m_inode->d_inode.direct[0], 0, sizeof(struct dirent) * DIRENTS_PER_BLK, &dir);

	// Create a list to store directory entries
	struct dirent entry_list[DIRENTS_PER_BLK];
	int num_entries = 0;

	// number of entries in the directory.
	int nr_entries = m_inode->d_inode.size / (sizeof(struct dirent));

	// Looping through the directory entries to find the entries and store them in a list.
	for (int a = 0; a < nr_entries; a++)
	{

		// If the name matches, return the inode number.
		// Also allows us to stat or more when we cd to parent directory.
		if (same_string(dir[a].name, name))
			return dir[a].inode;

		// printf("dirname: %s, name: %s, dir inode: %d\n", dir[a].name, name, dir[a].inode);

		// We iterate parent dir to find the name of the file. Since cd doesnt know what the name of entry is we have to look at parent.
		if (same_string(dir[a].name, PARENT_DIR))
		{
			// Reading the parrent inode table entry
			struct mem_inode *parent_inode = &m_inode_table[dir[a].inode];
			block_read_part(ino2blk(dir[a].inode), 0, sizeof(struct disk_inode), &parent_inode->d_inode);

			struct dirent dir_par[DIRENTS_PER_BLK];
			// Reading the parent data block to find the name of the file
			block_read_part(parent_inode->d_inode.direct[0], 0, sizeof(struct dirent) * DIRENTS_PER_BLK, &dir_par);

			// Iterating over parent directory entries and adding them to the list of entries.
			// Then we copy the name of the file to the list of entries, so we can keep track of them.
			for (int b = 0; b < nr_entries; b++)
			{

				// printf("\tdir_par[b].inode:%d, \tdir[a].inode:%d, \tdir_par bname:%s, \tdirnamea:%s  \n", dir_par[b].inode, dir[a].inode, dir_par[b].name, dir[a].name);

				struct dirent entry;
				entry.inode = dir_par[b].inode;
				// Copying the name of the file to the list of entries. To check if the name matches and to store them in list.
				bcopy(dir_par[b].name, entry.name, MAX_FILENAME_LEN + 1);

				entry_list[num_entries++] = entry;
			}
		}
		// Current add it to the list of entries. So can't make duplicate name in same directory.
		// But allowed to make duplicate names in different directories or in same directory depth +x own directory!
		else
		{
			// printf("\t############HITHITHIT ###############\n");
			struct dirent entry;
			entry.inode = dir[a].inode;
			// Copying the name of the file to the list of entries. To check if the name matches and to store them in list.
			bcopy(dir[a].name, entry.name, MAX_FILENAME_LEN + 1);

			entry_list[num_entries++] = entry;
		}
	}

	// Iterating over the directory entry list and return its inode number when there is a match.
	for (int i = 0; i < num_entries; i++)
	{
		// printf("\tentry_list[i].name:%s\tentry_list[i].inode:%d\tname:%s\n", entry_list[i].name, entry_list[i].inode, name);
		// If the name matches, return the inode number.
		if (same_string(entry_list[i].name, name))
			return entry_list[i].inode;
	}

	// If no matches found we return -1.
	return FSE_ERROR;
}
