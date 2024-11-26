// analyzedmtx.c

#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <dmtx.h>
#include <wand/MagickWand.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h> // For PATH_MAX
#include <ctype.h>  // For isspace

#define TRUE 1
#define FALSE 0

// Used to store the file names and result messages
#define MAX_FNAME_LENGTH PATH_MAX
#define MAX_MESSAGE_LENGTH 8000

// Data structure to store filename/message
typedef struct {
    char filename[MAX_FNAME_LENGTH];
    char message[MAX_MESSAGE_LENGTH];
} filedata;

// FD for shared memory object
int sharedfd;

// Ptr to shared memory object
filedata* sharedmem;

// Number of files in the directory
int numfiles = 0;

// Initialize MagickWand
int initialized = 0;
void initdmtx() {
    if (!initialized) {
        MagickWandGenesis();
        initialized = TRUE;
    }
}

// Close MagickWand
void closedmtx() {
    if (initialized) {
        MagickWandTerminus();
        initialized = FALSE;
    }
}

// Function to decode a Data Matrix image
char* scandmtx(char* filepath) {
    int width, height;
    unsigned char* pxl;
    MagickBooleanType success;
    DmtxImage* img;
    DmtxDecode* dec;
    DmtxRegion* reg;
    DmtxMessage* msg;
    char* result = NULL;
    MagickWand* wand;

    initdmtx();
    wand = NewMagickWand();

    if (MagickReadImage(wand, filepath) == MagickFalse) {
        fprintf(stderr, "Error reading image: %s\n", filepath);
        DestroyMagickWand(wand);
        return NULL;
    }

    width = MagickGetImageWidth(wand);
    height = MagickGetImageHeight(wand);
    pxl = (unsigned char *)malloc(3 * width * height * sizeof(unsigned char));

    success = MagickExportImagePixels(wand, 0, 0, width, height, "RGB", CharPixel, pxl);
    if (success == MagickFalse) {
        fprintf(stderr, "Error exporting image pixels: %s\n", filepath);
        free(pxl);
        DestroyMagickWand(wand);
        return NULL;
    }

    img = dmtxImageCreate(pxl, width, height, DmtxPack24bppRGB);
    if (img == NULL) {
        fprintf(stderr, "Error creating DmtxImage: %s\n", filepath);
        free(pxl);
        DestroyMagickWand(wand);
        return NULL;
    }

    dec = dmtxDecodeCreate(img, 1);
    if (dec == NULL) {
        fprintf(stderr, "Error creating DmtxDecode: %s\n", filepath);
        dmtxImageDestroy(&img);
        free(pxl);
        DestroyMagickWand(wand);
        return NULL;
    }

    reg = dmtxRegionFindNext(dec, NULL);
    if (reg != NULL) {
        msg = dmtxDecodeMatrixRegion(dec, reg, DmtxUndefined);
        if (msg != NULL) {
            result = strdup((char*)msg->output);
            dmtxMessageDestroy(&msg);
        } else {
            fprintf(stderr, "Error decoding matrix region: %s\n", filepath);
        }
        dmtxRegionDestroy(&reg);
    } else {
        fprintf(stderr, "No Data Matrix code found: %s\n", filepath);
    }

    dmtxDecodeDestroy(&dec);
    dmtxImageDestroy(&img);
    free(pxl);
    DestroyMagickWand(wand);

    return result;
}

// Generate a list of files in the directory
int generate_file_list(char* path) {
    DIR *dir;
    struct dirent *ent;
    numfiles = 0;

    if ((dir = opendir(path)) == NULL) {
        perror("opendir");
        return FALSE;
    }

    // First pass to count the number of files
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, ".png") != NULL) {
            numfiles++;
        }
    }
    closedir(dir);

    if (numfiles == 0) {
        fprintf(stderr, "No .png files found in directory: %s\n", path);
        return FALSE;
    }

    // Create shared memory object
    sharedfd = shm_open("/filelist", O_CREAT | O_RDWR, 0666);
    if (sharedfd < 0) {
        perror("shm_open");
        return FALSE;
    }

    // Resize shared memory object
    if (ftruncate(sharedfd, sizeof(filedata) * numfiles) < 0) {
        perror("ftruncate");
        shm_unlink("/filelist");
        return FALSE;
    }

    // Map shared memory object
    sharedmem = mmap(NULL, sizeof(filedata) * numfiles, PROT_READ | PROT_WRITE, MAP_SHARED, sharedfd, 0);
    if (sharedmem == MAP_FAILED) {
        perror("mmap");
        shm_unlink("/filelist");
        return FALSE;
    }

    // Second pass to store filenames
    dir = opendir(path);
    int index = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, ".png") != NULL) {
            // Compute the total length needed
            size_t total_length = strlen(path) + 1 + strlen(ent->d_name) + 1; // +1 for '/', +1 for null terminator

            if (total_length > MAX_FNAME_LENGTH) {
                fprintf(stderr, "File path too long: %s/%s\n", path, ent->d_name);
                continue; // Skip this file
            }

            snprintf(sharedmem[index].filename, MAX_FNAME_LENGTH, "%s/%s", path, ent->d_name);
            sharedmem[index].filename[MAX_FNAME_LENGTH - 1] = '\0'; // Ensure null-termination
            sharedmem[index].message[0] = '\0'; // Initialize message
            index++;
        }
    }
    closedir(dir);

    numfiles = index; // Update numfiles in case we skipped any files

    return TRUE;
}

// Sequential implementation
void generate_dmtx_seq() {
    filedata* filelist = sharedmem;

    for (int i = 0; i < numfiles; i++) {
        char* message = scandmtx(filelist[i].filename);
        if (message != NULL) {
            strncpy(filelist[i].message, message, MAX_MESSAGE_LENGTH - 1);
            filelist[i].message[MAX_MESSAGE_LENGTH - 1] = '\0';
            free(message);
        } else {
            // Do not store error messages
            filelist[i].message[0] = '\0';
        }
    }
    closedmtx();
}

// Parallel implementation
void generate_dmtx_par(int numprocesses) {
    filedata* filelist = sharedmem;
    int files_per_process = numfiles / numprocesses;
    int remaining_files = numfiles % numprocesses;

    for (int p = 0; p < numprocesses; p++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        if (pid == 0) {
            // Child process
            int start = p * files_per_process + (p < remaining_files ? p : remaining_files);
            int end = start + files_per_process + (p < remaining_files ? 1 : 0);

            for (int i = start; i < end; i++) {
                char* message = scandmtx(filelist[i].filename);
                if (message != NULL) {
                    strncpy(filelist[i].message, message, MAX_MESSAGE_LENGTH - 1);
                    filelist[i].message[MAX_MESSAGE_LENGTH - 1] = '\0';
                    free(message);
                } else {
                    // Do not store error messages
                    filelist[i].message[0] = '\0';
                }
            }
            exit(0);
        }
    }

    // Parent process waits for all children
    for (int i = 0; i < numprocesses; i++) {
        wait(NULL);
    }

    closedmtx();
}

// Function to trim leading and trailing whitespace
void trim(char* str) {
    char* end;
    char* start = str;

    // Trim leading space
    while (isspace((unsigned char)*start)) start++;

    if (*start == 0) {
        // All spaces
        str[0] = '\0';
        return;
    }

    // Trim trailing space
    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;

    // Write new null terminator
    *(end + 1) = '\0';

    // Move trimmed string back to str
    memmove(str, start, end - start + 2); // +1 for '\0', +1 for end position
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <#processes> <folder> <output file>\n", argv[0]);
        return 1;
    }

    int numprocesses = atoi(argv[1]);

    // Generate the file list
    int result = generate_file_list(argv[2]);
    if (result == FALSE) {
        fprintf(stderr, "Error generating file list.\n");
        return -1;
    }

    // Process files sequentially or in parallel
    if (numprocesses <= 0) {
        generate_dmtx_seq();
    } else {
        generate_dmtx_par(numprocesses);
    }

    // Write results to output file
    FILE* fp = fopen(argv[3], "w");
    if (fp == NULL) {
        perror("Error opening output file");
        munmap(sharedmem, sizeof(filedata) * numfiles);
        shm_unlink("/filelist");
        return -1;
    }

    filedata* filelist = sharedmem;
    for (int i = 0; i < numfiles; i++) {
        char* message = filelist[i].message;

        // Trim whitespace
        trim(message);

        // Skip empty messages
        if (message[0] == '\0') {
            continue;
        }

        fprintf(fp, "%s\n", message);
    }

    // Flush and close the file
    fflush(fp);
    fclose(fp);

    // Cleanup shared memory
    munmap(sharedmem, sizeof(filedata) * numfiles);
    shm_unlink("/filelist");

    return 0;
}
