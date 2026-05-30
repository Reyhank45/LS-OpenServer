#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

void clear_screen() {
    printf("\033[2J\033[H");
}

void print_header(const char *title) {
    clear_screen();
    printf("\033[44;37m"); // Blue background, white text
    printf("================================================================================\n");
    printf("  %s\n", title);
    printf("================================================================================\n");
    printf("\033[0m\n"); // Reset colors
}

void show_welcome();
void show_edition_selection();

void show_edition_selection() {
    int choice = 0;
    while (1) {
        print_header("Edition Selection");
        printf("\nSelect the edition of LS-OpenServer to install:\n\n");
        printf("1. LS-OpenServer (Standard)\n");
        printf("2. LS-OpenServer-LSR (Router)\n");
        printf("3. Go Back\n\n");
        printf("Select an edition [1-3]: ");
        
        char buf[16];
        if (fgets(buf, sizeof(buf), stdin) == NULL) {
            break;
        }
        choice = atoi(buf);
        
        if (choice == 1 || choice == 2) {
            return;
        } else if (choice == 3) {
            show_welcome();
            return;
        }
    }
}

void show_disk_selection() {
    int choice = 0;
    while (1) {
        print_header("Disk Selection");
        printf("\nSelect the disk to install LS-OpenServer onto:\n\n");
        printf("1. ada0 (50 GB Virtual Disk)\n");
        printf("2. ada1 (100 GB Virtual Disk)\n");
        printf("3. Go Back\n\n");
        printf("Select a disk [1-3]: ");
        
        char buf[16];
        if (fgets(buf, sizeof(buf), stdin) == NULL) {
            break;
        }
        choice = atoi(buf);
        
        if (choice == 1 || choice == 2) {
            return;
        } else if (choice == 3) {
            show_edition_selection();
            return;
        }
    }
}

void show_welcome() {
    int choice = 0;
    while (1) {
        print_header("LS-OpenServer Installer");
        printf("\nWelcome to the LS-OpenServer setup.\n\n");
        printf("1. Install LS-OpenServer\n");
        printf("2. Boot Live Environment (Drop to Shell)\n");
        printf("3. Reboot\n\n");
        printf("Select an option [1-3]: ");
        
        char buf[16];
        if (fgets(buf, sizeof(buf), stdin) == NULL) {
            break;
        }
        choice = atoi(buf);
        
        if (choice == 1) {
            show_edition_selection();
            show_disk_selection();
            return; // Proceed to installation
        } else if (choice == 2) {
            exit(0); // Exit installer, init will drop to shell
        } else if (choice == 3) {
            printf("\nRebooting...\n");
            sleep(1);
            exit(0);
        }
    }
}

void show_progress() {
    print_header("Installing...");
    printf("\nExtracting base system files...\n\n");
    
    int total_width = 50;
    for (int i = 0; i <= total_width; i++) {
        printf("\r[");
        for (int j = 0; j < total_width; j++) {
            if (j < i) printf("#");
            else printf(" ");
        }
        printf("] %d%%", (i * 100) / total_width);
        fflush(stdout);
        usleep(50000); // 50ms per step
    }
    printf("\n\nInstallation complete!\n");
    printf("Press Enter to boot the new system or drop to shell...");
    char buf[16];
    fgets(buf, sizeof(buf), stdin);
    exit(0);
}

int main() {
    show_welcome();
    show_progress();
    return 0;
}
