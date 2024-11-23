#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <dmtx.h>
#include <wand/magick-wand.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>

#define TRUE 1
#define FALSE 0

// Used to store the file names and result messages
#define MAX_FNAME_LENGTH 256
#define MAX_MESSAGE_LENGTH 8000

// Data structure to store filename/message
typedef struct {
    char filename[MAX_FNAME_LENGTH];
    char message[MAX_MESSAGE_LENGTH];
} filedata;

// FD for shared memory object
int sharedfd;

// Ptr to shared memory object
char* sharedmem;

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
        DestroyMagickWand(wand);
        return NULL;
    }

    width = MagickGetImageWidth(wand);
    height = MagickGetImageHeight(wand);
    pxl = (unsigned char *)malloc(3 * width * height * sizeof(unsigned char));

    success = MagickExportImagePixels(wand, 0, 0, width, height, "RGB", CharPixel, pxl);
    if (success == MagickFalse) {
        free(pxl);
        DestroyMagickWand(wand);
        return NULL;
    }

    img = dmtxImageCreate(pxl, width, height, DmtxPack24bppRGB);
    if (img == NULL) {
        free(pxl);
        DestroyMagickWand(wand);
        return NULL;
    }

    dec = dmtxDecodeCreate(img, 1);
    reg = dmtxRegionFindNext(dec, NULL);
    if (reg != NULL) {
        msg = dmtxDecodeMatrixRegion(dec, reg, DmtxUndefined);
        if (msg != NULL) {
            result = strdup((char*)msg->output);
            dmtxMessageDestroy(&msg);
        }
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

    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, ".png") != NULL) {
            numfiles++;
        }
    }
    closedir(dir);

    sharedfd = shm_open("filelist", O_CREAT | O_RDWR, 0666);
    if (sharedfd < 0) {
        perror("shm_open");
        return FALSE;
    }

    if (ftruncate(sharedfd, sizeof(filedata) * numfiles) < 0) {
        perror("ftruncate");
        return FALSE;
    }

    sharedmem = mmap(NULL, sizeof(filedata) * numfiles, PROT_READ | PROT_WRITE, MAP_SHARED, sharedfd, 0);
    if (sharedmem == MAP_FAILED) {
        perror("mmap");
        return FALSE;
    }

    filedata* filelist = (filedata*)sharedmem;
    dir = opendir(path);
    int index = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, ".png") != NULL) {
            snprintf(filelist[index].filename, MAX_FNAME_LENGTH, "%s/%s", path, ent->d_name);
            index++;
        }
    }
    closedir(dir);

    return TRUE;
}

// Sequential implementation
void generate_dmtx_seq() {
    filedata* filelist = (filedata*)sharedmem;

    for (int i = 0; i < numfiles; i++) {
        char* message = scandmtx(filelist[i].filename);
        if (message != NULL) {
            strncpy(filelist[i].message, message, MAX_MESSAGE_LENGTH - 1);
            free(message);
        } else {
            strcpy(filelist[i].message, "Error decoding file");
        }
    }
    closedmtx();
}

// Parallel implementation
void generate_dmtx_par(int numprocesses) {
    filedata* filelist = (filedata*)sharedmem;
    int files_per_process = numfiles / numprocesses;
    sem_t* semaphore = sem_open("dmtxsem", O_CREAT, 0666, 1);

    for (int p = 0; p < numprocesses; p++) {
        if (fork() == 0) {
            int start = p * files_per_process;
            int end = (p == numprocesses - 1) ? numfiles : start + files_per_process;

            for (int i = start; i < end; i++) {
                char* message = scandmtx(filelist[i].filename);
                sem_wait(semaphore);
                if (message != NULL) {
                    strncpy(filelist[i].message, message, MAX_MESSAGE_LENGTH - 1);
                    free(message);
                } else {
                    strcpy(filelist[i].message, "Error decoding file");
                }
                sem_post(semaphore);
            }
            sem_close(semaphore);
            exit(0);
        }
    }

    for (int i = 0; i < numprocesses; i++) {
        wait(NULL);
    }

    sem_close(semaphore);
    sem_unlink("dmtxsem");
    closedmtx();
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <#processes> <folder> <output file>\n", argv[0]);
        return 1;
    }

    int numprocesses = atoi(argv[1]);

    int result = generate_file_list(argv[2]);
    if (result == FALSE)
        return -1;

    if (numprocesses == 0)
        generate_dmtx_seq();
    else
        generate_dmtx_par(numprocesses);

    FILE* fp = fopen(argv[3], "w");
    if (fp == NULL) {
        perror("fopen");
        return -1;
    }

    filedata* filelist = (filedata*)sharedmem;
    for (int i = 0; i < numfiles; i++) {
        fprintf(fp, "%s\n", filelist[i].message);
    }
    fclose(fp);

    munmap(sharedmem, sizeof(filedata) * numfiles);
    shm_unlink("filelist");

    return 0;
}
