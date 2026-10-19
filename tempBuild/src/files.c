#include <stdio.h>
#include <dirent.h>

int main(){
    DIR *dir;
    struct dirent *entry;

    dir = opendir(".");
    if (dir == NULL){
        printf("Failed to open directory\n");
        return 1;
    }

    while ((entry = readdir(dir)) != NULL){
        printf("%s\n", entry->d_name);
    }

    if (closedir(dir) != 0){
        printf("Failed to close directory\n");
        return 1;
    }

    return 0;
}