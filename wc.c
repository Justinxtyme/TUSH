//word_counter.c
/* WORD COUNTER FROM SCRATCH AS A FUCKING NOOB. THIS PROGRAM RETURNS CHAR,
WORD, AND LINE COUNT. AND YEAH. THATS ABOUT IT    */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
int word_counter(char* file) {
    printf("WORD COUNTER\n");
    FILE *fp = fopen(file, "r");
    if (fp == NULL) {
        printf("Error opening file\n");
        return 1;
    }
    //char *p = *fp;
    int chars = 0;
    int words = 0;
    int lines = 0;
    int c;
    int last = 0;
    
    printf("Variables intialized\n");
    
    while ((c = fgetc(fp)) != EOF) {
        //char c = fgetc(fp);
        //printf("assigned %c\n", c);
        if (isspace(c) && !isspace(last) && (last) != '\n') {
            words++;
            last = c;
            continue;
        }
        if (c == '\n') {
            lines++;
            last = c;
            continue;
        } else {
            chars++;
            last = c;
            continue;
        }
    }
    printf("Total chars: %d\n", chars);
    printf("Total words: %d\n", words);
    printf("Total lines: %d\n", lines);
    return 0;
}   


int main() {
    char filename[50];
    printf("Enter the name of the file: ");
    scanf("%s", filename);
    printf("scanning filename\n");
    printf("filename = %s\n", filename);
    printf("Running Word Counter on: %s\n", filename);
    word_counter(filename);
    return 0;
}
