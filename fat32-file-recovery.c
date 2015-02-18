//  FAT32 file recovery

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <stdbool.h>

#define DEBUG 0


// This macro is to read 4 bytes in little endian order at address a
#define UINT_AT(a) (*((unsigned int *)(a)))
// Read unsigned 2 bytes
#define USHORT_AT(a) (*((unsigned short *)(a)))

void exit_with_usage(char *cmd_name)
{
	printf ("Usage: %s -d [device filename] [other arguments]\n", cmd_name);
	printf ("-i                    Print boot sector information\n");
	printf ("-l                    List all the directory entries\n");
	printf ("-r filename           File recovery\n");
	
	exit(-1);
}

char *device_filename = NULL;
char *recovery_filename = NULL;
enum { PRINT_BOOT_SECTOR, LIST_ALL_DIR, FILE_RECOVER, UNSET  } mode = UNSET;

void parseLine(int argc, char *argv[])
{
	int i;
	
	// At least 4 arguments: ./recover -d [device filename] [other arguments]
	if (argc < 4) exit_with_usage(argv[0]);
	
	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-d"))
		{
			if (++i >= argc || device_filename != NULL)
			{
				exit_with_usage(argv[0]);
			}
			else
			{
				device_filename = malloc(sizeof(char) * (strlen(argv[i])+1));
				strcpy(device_filename, argv[i]);
			}
			continue;
		}
		else if (!strcmp(argv[i], "-i"))
		{
		    if (mode != UNSET && mode != PRINT_BOOT_SECTOR) exit_with_usage(argv[0]);
			
			mode = PRINT_BOOT_SECTOR;
			continue;
		}
		else if (!strcmp(argv[i], "-l"))
		{
			if (mode != UNSET && mode != LIST_ALL_DIR) exit_with_usage(argv[0]);
			mode = LIST_ALL_DIR;
			continue;
		}
		else if (!strcmp(argv[i], "-r"))
		{
			if (mode != UNSET) exit_with_usage(argv[0]);
			
			if (++i >= argc || recovery_filename != NULL) exit_with_usage(argv[0]);
			else
			{
				recovery_filename = malloc(sizeof(char) * (strlen(argv[i])+1));
				strcpy(recovery_filename, argv[i]);
			}
			
			mode = FILE_RECOVER;
			continue;
		}
		else
		{
			exit_with_usage(argv[0]);
		}
	}
	
	// Error if there are arguments left or mode unset
	if (mode == UNSET ) exit_with_usage(argv[0]);
}

//
FILE *dev_file;


// Disk information.
int num_of_fats, bytes_per_sector, sectors_per_cluster, reserved_sectors,
sectors_per_fat, root_cluster;

void init_diskinfo()
{
	char buf[0x30]; // a 30h bytes buffer for reading;
	
	fseek (dev_file, 0x00B, SEEK_SET);
	fread (buf, 1, 0x30, dev_file);
	
    
	// Note the values in buf are in little-endian
	bytes_per_sector = buf[0] + (buf[1] << 8);
	sectors_per_cluster = buf[2];
	reserved_sectors = buf[3] + (buf[4] << 8);
	num_of_fats = buf[5];
	sectors_per_fat = *((unsigned int *)(buf+0x19));
	root_cluster = *((unsigned int *)(buf+0x21));
	
}


unsigned int *FAT_table_on_disk;
unsigned int *FAT_table;


void init_FAT_table()
{
	
	unsigned int i;
	FAT_table_on_disk = malloc(bytes_per_sector * sectors_per_fat);
	FAT_table = malloc(bytes_per_sector * sectors_per_fat);
	
	fseek (dev_file, reserved_sectors * bytes_per_sector, SEEK_SET);
	fread (FAT_table_on_disk, 1, bytes_per_sector * sectors_per_fat, dev_file);
	
	if (DEBUG)
	{
		for (i = 0; i < num_of_fats; i++)
		{
			printf ("FAT table %d location: %x\n", i+1, reserved_sectors * bytes_per_sector + i * bytes_per_sector * sectors_per_fat);
		}
	}
	for (i = 0; i < (bytes_per_sector * sectors_per_fat)/4; i++)
	{
		// Remember FAT32 is actually FAT-28: the uppermost 4 bits are ignored.
		FAT_table[i] = FAT_table_on_disk[i] & 0x0FFFFFFF;
	}
}

// Write FAT table to disk
void sync_FAT_table()
{
	int i;
	
	fseek (dev_file, reserved_sectors * bytes_per_sector, SEEK_SET);
	
	// Merge the upper 4-bits of original entry
	for (i = 0; i < (bytes_per_sector * sectors_per_fat)/4; i++)
	{
		FAT_table_on_disk[i] = (FAT_table_on_disk[i] & 0xF0000000) | (FAT_table[i] & 0x0FFFFFFF);
	}
	
	for (i = 0; i < num_of_fats; i++)
	{
		fwrite(FAT_table_on_disk, 1, bytes_per_sector * sectors_per_fat, dev_file);
	}
}

int isEOC(unsigned int fat_entry)
{
	return ( (0x0FFFFFF8 <= fat_entry)  && (fat_entry <= 0x0FFFFFFF) );
}


#define DISK_SEEK_CLUSTER  1
#define DISK_SEEK_CUR 2

unsigned int seek_cluster = 0x0FFFFFFF;
unsigned int seek_cluster_offset = 0;

// Returns -1 if EOC is reached
// Should not seek to cluster 0 or 1. in that case, return 0
int disk_seek(unsigned int number, int flag)
{
	unsigned int cur_cluster;
	unsigned int cluster_size = sectors_per_cluster * bytes_per_sector;
	unsigned int target_loc_to_seek_cluster;
#define CLUSTER_LOC(x) (( ((x) -2)* sectors_per_cluster  + reserved_sectors + num_of_fats * sectors_per_fat) * bytes_per_sector)
	switch (flag)
	{
        case DISK_SEEK_CLUSTER:
            // Should not seek to cluster 0 or 1
            if (number == 0 || number == 1)
            {
                return 0;
            }
            seek_cluster = number;
            seek_cluster_offset = 0;
            
            if (!isEOC(seek_cluster))
            {
                if (DEBUG) {printf ("seek(C) to %x\n", CLUSTER_LOC(seek_cluster));}
                return fseek (dev_file, CLUSTER_LOC(seek_cluster), SEEK_SET);
            }
            else return -1;
            
            break;
            
        case DISK_SEEK_CUR:
            if (isEOC(seek_cluster)) return -1;
            
            target_loc_to_seek_cluster = seek_cluster_offset + number;
            cur_cluster = seek_cluster;
            
            // Move cluster if the seek location is greater than the current cluster
            if ( target_loc_to_seek_cluster  >= cluster_size)
            {
                for (cur_cluster = FAT_table[seek_cluster]; !isEOC(cur_cluster); cur_cluster = FAT_table[cur_cluster])
                {
                    target_loc_to_seek_cluster -= cluster_size;
                    if (target_loc_to_seek_cluster < cluster_size)
                        break;
                }
            }
            
            seek_cluster = cur_cluster;
            seek_cluster_offset = target_loc_to_seek_cluster;
			
            if (isEOC(seek_cluster))
                return -1;
            else
            {
                if (DEBUG) { printf ("seek to %x\n", CLUSTER_LOC(seek_cluster)+seek_cluster_offset); }
                return fseek (dev_file, CLUSTER_LOC(seek_cluster)+seek_cluster_offset, SEEK_SET);
            }
            break;
        default:
            return -1;
	}
}

// Returns -1 if EOC is reached
int disk_read(unsigned char *buf, unsigned int bytes_to_read)
{
	unsigned int cur_cluster;
	unsigned int total_bytes_read = 0;
	
	unsigned int body_count = 0;
	unsigned int head_count = 0;
	unsigned int tail_count = 0;
	
	unsigned int cluster_size = sectors_per_cluster * bytes_per_sector;
	
	unsigned int end_loc_to_seek_cluster = seek_cluster_offset + bytes_to_read -1;
    
	unsigned int bytes_inside_first_cluster = 0;
	
	
	if (bytes_to_read == 0) return 0;
	if (isEOC(seek_cluster)) return -1;
	
	if (seek_cluster_offset == cluster_size) disk_seek(FAT_table[seek_cluster], DISK_SEEK_CLUSTER);
	
	
	// We perform a 3-phase read:
	// read the data "head" inside seek_cluster ->
	// read the data "body" in several clusters ->
	// read the data "tail" inside the last cluster
	
	// "head"
	if (end_loc_to_seek_cluster < cluster_size)
	{
		fread (buf, 1, bytes_to_read, dev_file);
		disk_seek(bytes_to_read, DISK_SEEK_CUR);
		
		head_count++;
		return bytes_to_read;
	}
	else
	{
		bytes_inside_first_cluster = cluster_size-seek_cluster_offset;
        
		fread (buf, 1, bytes_inside_first_cluster, dev_file);
		// This makes it seeks to the next cluster. Note: this moves the seek_cluster! MAKE SURE YOU WANT to use its value
		disk_seek(bytes_inside_first_cluster, DISK_SEEK_CUR);
		
		buf += bytes_inside_first_cluster;
		total_bytes_read += bytes_inside_first_cluster;
		end_loc_to_seek_cluster -= bytes_inside_first_cluster;
		
		head_count++;
	}
	
	// "Body" & "tail"
	for (cur_cluster = seek_cluster; !isEOC(cur_cluster); cur_cluster = FAT_table[cur_cluster])
	{
		disk_seek(cur_cluster, DISK_SEEK_CLUSTER);
		
		// Tail
		if(end_loc_to_seek_cluster < cluster_size)
		{
			fread (buf, 1, end_loc_to_seek_cluster+1, dev_file);
			total_bytes_read += end_loc_to_seek_cluster+1;
            
			disk_seek(end_loc_to_seek_cluster+1, DISK_SEEK_CUR);
			
			buf += end_loc_to_seek_cluster+1;
			tail_count++;
			
			break;
		}
		else  // Body
		{
			fread (buf, 1, cluster_size, dev_file);
			total_bytes_read += cluster_size;
			
			buf += cluster_size;
			
			body_count++;
		}
		
		end_loc_to_seek_cluster -= cluster_size;
	}
    
	if(DEBUG) { printf ("Head: %d times, Body: %d times, Tail: %d times", head_count, body_count, tail_count); }
	// Data remaining but EOC already
	// if (end_loc_to_seek_cluster >= cluster_size) return -1;
	
	return total_bytes_read;
}

// Returns the next start_row_number for printing
int list_dir(char *prefix_str, int start_row_number, unsigned int start_cluster)
{
	int i,j;
	unsigned char dir_entry[32];
	char filename[20] = {0};
	char *cur_loc_prefix_str;
	unsigned int file_size, file_start_cluster;
	unsigned int backup_seek_cluster, backup_seek_cluster_offset;
	
	disk_seek(start_cluster, DISK_SEEK_CLUSTER);
	while ( disk_read(dir_entry, 32) > 0 )
	{
        
        
		// LFN: skip it
		if (dir_entry[0x0B] == 0x0f)
			continue;
		
		// Empty entries or deleted entries: skip
		if (dir_entry[0] == 0x000 || dir_entry[0] == 0xE5)
			continue;
		
		
		// Filename: skip all the space padding
		for (i = 0; i < 8; i++)
		{
			if (dir_entry[i] == ' ')
				break;
			filename[i] = dir_entry[i];
		}
		
		// Extension
		if (dir_entry[8] != ' ')
		{
			filename[i++] = '.';
			for (j = 8; j < 11; j++)
			{
				
				if (dir_entry[j] == ' ')
					break;
				filename[i++] = dir_entry[j];
			}
		}
		
		// Special care for directory '.', '..' and normal directory
		if (dir_entry[0x0B] == 0x10 && dir_entry[0] != 0x2E)
		{
			filename[i++] = '/';
		}
		
		filename[i] = '\0';
		
		
		file_start_cluster = USHORT_AT(dir_entry+0x1A) + (USHORT_AT(dir_entry+0x14) << (16));
		file_size = UINT_AT(dir_entry+0x1C);
		
		printf ("%d, %s%s, %u, %u\n", start_row_number++, prefix_str, filename, file_size, file_start_cluster);
		
		// Subdirectory and filename not '.' or '..'
		if (dir_entry[0x0B] == 0x10 && dir_entry[0] != 0x2E)
		{
			cur_loc_prefix_str = malloc(strlen(prefix_str)+strlen(filename) + 1);  // +1 and '\0'
			cur_loc_prefix_str[0] = '\0';
			strcat(cur_loc_prefix_str, prefix_str);
			strcat(cur_loc_prefix_str, filename);
			
			// backup our seek location first
			backup_seek_cluster = seek_cluster;
			backup_seek_cluster_offset = seek_cluster_offset;
			
			start_row_number = list_dir (cur_loc_prefix_str, start_row_number, file_start_cluster);
			
			// Restore seek
			disk_seek (backup_seek_cluster, DISK_SEEK_CLUSTER);
			disk_seek (backup_seek_cluster_offset, DISK_SEEK_CUR);
		}
	}
	
	return start_row_number;
}

void list_all_dir()
{
	init_diskinfo();
	init_FAT_table();
	
	list_dir("", 1, root_cluster);
}

bool recovered, recover_failed;

void file_recover_core(char *prefix_str, unsigned int start_cluster, char *namepart, char *extension)
{
    int i, j, k;
    unsigned int backup_seek_cluster, backup_seek_cluster_offset;
    unsigned int file_size, file_start_cluster;
    unsigned char dir_entry[32];
    char *cur_loc_prefix_str, filename[20] = {0};
    
    // Recover file
    disk_seek(start_cluster, DISK_SEEK_CLUSTER);
    while (disk_read(dir_entry, 32) > 0 && !recovered && !recover_failed) {
        
        // Filename: skip all the space padding
		for (j = 0; j < 8; j++)
		{
			if (dir_entry[j] == ' ')
				break;
			filename[j] = dir_entry[j];
		}
		
		// Extension
		if (dir_entry[8] != ' ')
		{
			filename[j++] = '.';
			for (k = 8; k < 11; k++)
			{
				
				if (dir_entry[k] == ' ')
					break;
				filename[j++] = dir_entry[k];
			}
		}
		
		filename[j] = '\0';
        
        if (dir_entry[0] == 0xE5) {
            
            for (i = 1; i < 8; i++) {
                if (dir_entry[i] != namepart[i]) {
                    break;
                }
            }
            if (i == 8) {
                for (; i < 11; i++) {
                    if (dir_entry[i] != extension[i-8]) {
                        break;
                    }
                }
            }
            if (i == 11) {
                
                file_start_cluster = USHORT_AT(dir_entry+0x1A) + (USHORT_AT(dir_entry+0x14) << (16));
                file_size = UINT_AT(dir_entry+0x1C);
                
                // Fix file name
                fseek(dev_file, -32, SEEK_CUR);
                dir_entry[0] = namepart[0];
                fwrite(dir_entry, 1, 1, dev_file);
                
                // Fix FAT
                for (j = 1; j < (file_size + (bytes_per_sector * sectors_per_cluster) - 1) / (bytes_per_sector * sectors_per_cluster); j++, file_start_cluster++) {
                    if (FAT_table[file_start_cluster]) {
                        printf("%s:  error - fail to recover", recovery_filename);
                        recover_failed = true;
                        break;
                    }
                    FAT_table[file_start_cluster] = file_start_cluster+1;
                }
                if (recover_failed) {
                    break;
                }
                FAT_table[file_start_cluster] = EOF;
                sync_FAT_table();
                printf("%s: recovered in %s\n", recovery_filename, prefix_str);
                recovered = true;
                break;
                
            }
        }
        
        // Subdirectory and filename not '.' or '..'
        if (dir_entry[0x0B] == 0x10 && dir_entry[0] != 0x2E) {
			
            cur_loc_prefix_str = malloc(strlen(prefix_str)+strlen(filename) + 1);  // +1 and '\0'
			cur_loc_prefix_str[0] = '\0';
			strcat(cur_loc_prefix_str, prefix_str);
			strcat(cur_loc_prefix_str, filename);
            
			// backup our seek location first
			backup_seek_cluster = seek_cluster;
			backup_seek_cluster_offset = seek_cluster_offset;
            file_start_cluster = USHORT_AT(dir_entry+0x1A) + (USHORT_AT(dir_entry+0x14) << (16));
			
            file_recover_core(cur_loc_prefix_str, file_start_cluster, namepart, extension);
            
			// Restore seek
			disk_seek (backup_seek_cluster, DISK_SEEK_CLUSTER);
			disk_seek (backup_seek_cluster_offset, DISK_SEEK_CUR);
            
        }
        
    }
    
}

void file_recover()
{
	init_diskinfo();
	init_FAT_table();
	
    int i;
	char *namepart, *extension, *ptr, *token;
    char backup_recovery_filename[13];
    
    namepart = (char *)malloc(sizeof(char) * 9);
    extension = (char *)malloc(sizeof(char) * 4);
    
    // Capitalize recovery_filename
    for (ptr = recovery_filename; *ptr; ptr++) {
        *ptr = toupper(*ptr);
    }
    
    strcpy(backup_recovery_filename, recovery_filename);
    
    // Obtain namepart and extension from recovery_filename
    if (strchr(recovery_filename, '.')) {
        for (i = 0, token = strtok(recovery_filename, "."); token; i++, token = strtok(NULL, ".")) {
            if (i) {
                strcpy(extension, token);
                while (strlen(extension) < 3) {
                    strcat(extension, " ");
                }
            } else {
                strcpy(namepart, token);
                while (strlen(namepart) < 8) {
                    strcat(namepart, " ");
                }
            }
        }
    } else {
        strcpy(namepart, recovery_filename);
        while (strlen(namepart) < 8) {
            strcat(namepart, " ");
        }
        strcpy(extension, "   ");
    }
    
    strcpy(recovery_filename, backup_recovery_filename);
    
    recovered = recover_failed = false;
    file_recover_core("/", root_cluster, namepart, extension); // start_cluster = 2
    if (!recovered && !recover_failed) {
        printf("%s: error - file not found\n", recovery_filename);
    }
	
}

void print_boot_sector()
{
	init_diskinfo();
	printf ("Number of FATs = %d\n", num_of_fats);
	printf ("Number of bytes per sector = %d\n", bytes_per_sector);
	printf ("Number of sectors per cluster = %d\n", sectors_per_cluster);
	printf ("Number of reserved sectors = %d\n", reserved_sectors);
	
	if (DEBUG)
	{
		printf ("Sectors per FAT: %d\n", sectors_per_fat);
		printf ("Root Cluster: %d\n", root_cluster);
	}
}

int main(int argc, char *argv[])
{
	parseLine(argc, argv);
	
	
	if ((dev_file = fopen (device_filename, "rb+")) == NULL)
	{
		fprintf(stderr, "Error when opening the file.\n");
		exit(-1);
	}
	
	
	switch(mode)
	{
		case PRINT_BOOT_SECTOR:
			print_boot_sector();
			break;
		case LIST_ALL_DIR:
			list_all_dir();
			break;
		case FILE_RECOVER:
			file_recover();
			break;
		default:
			break;
	}
	fclose (dev_file);
	
	return 0;
}
