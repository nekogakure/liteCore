#include <stdio.h>

FILE __stdin_FILE = { 0 };
FILE __stdout_FILE = { 0 };
FILE __stderr_FILE = { 0 };

FILE *stdin = &__stdin_FILE;
FILE *stdout = &__stdout_FILE;
FILE *stderr = &__stderr_FILE;

// 珍しくきれいだぞ！！！！褒めろ！！！！（内容がうっすいからですだまりなさい）