#include <windows.h>
#include <stdio.h>
#include "elf_load.h"
static void log_line(const char *s){puts(s);fflush(stdout);}
int main(int argc,char **argv){wchar_t dir[1024];if(argc!=2)return 2;MultiByteToWideChar(CP_UTF8,0,argv[1],-1,dir,1024);CreateDirectoryW(L"out\\elf-state",NULL);return elf_load_test(dir,L"out\\elf-state",log_line);}
