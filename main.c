#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <unistd.h>
#endif

#define MAX_TASKS 50
#define MAX_TITLE 120
#define MAX_SKIPPED_DATES 100
#define ONE_TIME (-1)
#define ALL_TASKS (-2)
#define TASK_FILE "todo.txt"
#define TEMP_FILE "todo.tmp"
#define BACKUP_FILE "todo.bak"

typedef struct {
    char title[MAX_TITLE + 1];
    int date;
    int weekday;
    int skipped_dates[MAX_SKIPPED_DATES];
    int skipped_count;
} Task;

static const char *const weekdays[] = {
    "일요일", "월요일", "화요일", "수요일", "목요일", "금요일", "토요일"
};

/* 터미널 화면과 키보드 입력 */

#if defined(__unix__) || defined(__APPLE__)
#define LEAVE_SCREEN "\x1b[?1049l"

static void leave_screen(void)
{
    fputs(LEAVE_SCREEN, stdout);
    fflush(stdout);
}

static void leave_screen_on_signal(int signum)
{
    /* Ctrl+C 처리 중에는 printf 대신 신호 처리에서 사용 가능한 write를 쓴다. */
    (void)write(STDOUT_FILENO, LEAVE_SCREEN, sizeof LEAVE_SCREEN - 1);
    _Exit(128 + signum);
}
#endif

static int use_alternate_screen(void)
{
#if defined(__unix__) || defined(__APPLE__)
    const char *term = getenv("TERM");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        term == NULL || term[0] == '\0' || strcmp(term, "dumb") == 0 ||
        atexit(leave_screen) != 0) {
        return 0;
    }

    signal(SIGINT, leave_screen_on_signal);
    signal(SIGTERM, leave_screen_on_signal);

    /* 터미널의 별도 화면을 열어 기존 셸 화면을 잠시 가린다. */
    fputs("\x1b[?1049h", stdout);
    fflush(stdout);
    return 1;
#else
    return 0;
#endif
}

static void clear_menu_screen(int alternate_screen)
{
    if (alternate_screen) {
        fputs("\x1b[2J\x1b[H", stdout);
    }
}

/* 줄이 버퍼보다 길면 남은 입력도 읽어 다음 질문에 섞이지 않게 한다. */
static int read_line(char text[], int size)
{
    fflush(stdout);
    if (fgets(text, size, stdin) == NULL) {
        return 0;
    }

    size_t end = strcspn(text, "\n");
    if (text[end] == '\n') {
        text[end] = '\0';
        return 1;
    }
    if (feof(stdin)) {
        return 1;
    }

    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {
    }
    return -1;
}

static int read_choice(const char prompt[], int min, int max)
{
    char input[32];

    for (;;) {
        printf("%s", prompt);
        int result = read_line(input, sizeof input);
        if (result == 0) {
            return -1;
        }
        if (result == 1) {
            char *end;
            long number = strtol(input, &end, 10);
            if (end != input && end[strspn(end, " \t\r")] == '\0' &&
                number >= min && number <= max) {
                return (int)number;
            }
        }
        printf("%d부터 %d 사이의 번호를 입력하세요.\n", min, max);
    }
}

static int wait_for_menu(int alternate_screen)
{
    if (!alternate_screen) {
        return 1;
    }

    char input[16];
    printf("\nEnter를 누르면 메인 메뉴로 돌아갑니다. ");
    return read_line(input, sizeof input) != 0;
}

/* 날짜와 할 일 목록 */

static int date_number(const struct tm *day)
{
    return (day->tm_year + 1900) * 10000 + (day->tm_mon + 1) * 100 + day->tm_mday;
}

static void print_date(int date)
{
    printf("%04d-%02d-%02d", date / 10000, date / 100 % 100, date % 100);
}

static int task_matches_date(const Task *task, int date, int weekday)
{
    if (task->weekday == ONE_TIME) {
        return task->date == date;
    }
    if (task->weekday != weekday) {
        return 0;
    }
    for (int i = 0; i < task->skipped_count; i++) {
        if (task->skipped_dates[i] == date) {
            return 0;
        }
    }
    return 1;
}

static void show_tasks(const Task tasks[], int count, int date, int weekday)
{
    int shown = 0;

    for (int i = 0; i < count; i++) {
        const Task *task = &tasks[i];
        if (weekday != ALL_TASKS && !task_matches_date(task, date, weekday)) {
            continue;
        }

        printf("%d. %s", i + 1, task->title);
        if (task->weekday == ONE_TIME) {
            printf(" [");
            print_date(task->date);
            printf("]\n");
        } else {
            printf(" [매주 %s]\n", weekdays[task->weekday]);
        }
        shown++;
    }

    if (shown == 0) {
        puts("할 일이 없습니다.");
    }
}

/* 저장 파일 읽기와 쓰기 */

/* 기존 항목은 날짜와 요일 뒤에 제목이 온다. R은 건너뛸 날짜가 있는 반복 항목이다. */
static int load_task_record(FILE *file, const char header[], Task *task)
{
    if (strchr(header, '\n') == NULL) {
        return 0;
    }

    char extra;
    int fields;
    task->skipped_count = 0;
    if (header[0] == 'R') {
        task->date = 0;
        fields = sscanf(header, "R %2d %3d %c", &task->weekday, &task->skipped_count, &extra);
    } else {
        fields = sscanf(header, "%8d %2d %c", &task->date, &task->weekday, &extra);
    }
    if (fields != 2 || task->weekday < ONE_TIME || task->weekday > 6 ||
        (task->weekday == ONE_TIME ? task->date <= 0 : task->date != 0) ||
        task->skipped_count < 0 || task->skipped_count > MAX_SKIPPED_DATES) {
        return 0;
    }

    char title[MAX_TITLE + 2];
    if (fgets(title, sizeof title, file) == NULL) {
        return 0;
    }
    size_t end = strcspn(title, "\n");
    if (end == 0 || title[end] != '\n') {
        return 0;
    }
    title[end] = '\0';
    strcpy(task->title, title);

    char line[16];
    for (int i = 0; i < task->skipped_count; i++) {
        if (fgets(line, sizeof line, file) == NULL || strchr(line, '\n') == NULL ||
            sscanf(line, "%8d %c", &task->skipped_dates[i], &extra) != 1 ||
            task->skipped_dates[i] <= 0) {
            return 0;
        }
    }
    return 1;
}

static int load_tasks(Task tasks[])
{
    FILE *file = fopen(TASK_FILE, "r");
    if (file == NULL && errno == ENOENT) {
        if (rename(BACKUP_FILE, TASK_FILE) == 0) {
            file = fopen(TASK_FILE, "r");
        } else if (errno == ENOENT) {
            return 0;
        }
    }
    if (file == NULL) {
        return -1;
    }

    int count = 0;
    char header[32];

    while (fgets(header, sizeof header, file) != NULL) {
        if (count == MAX_TASKS || !load_task_record(file, header, &tasks[count])) {
            count = -1;
            break;
        }
        count++;
    }

    if (ferror(file)) {
        count = -1;
    }
    if (fclose(file) != 0) {
        count = -1;
    }
    return count;
}

static int replace_task_file(void)
{
    if (rename(TEMP_FILE, TASK_FILE) == 0) {
        return 1;
    }

    /* 기존 파일 위로 이름을 바꿀 수 없는 환경에서는 먼저 백업한다. */
    if ((remove(BACKUP_FILE) != 0 && errno != ENOENT) ||
        rename(TASK_FILE, BACKUP_FILE) != 0) {
        return 0;
    }
    if (rename(TEMP_FILE, TASK_FILE) != 0) {
        rename(BACKUP_FILE, TASK_FILE);
        return 0;
    }
    remove(BACKUP_FILE);
    return 1;
}

static int write_task_record(FILE *file, const Task *task)
{
    if (task->skipped_count == 0) {
        return fprintf(file, "%d %d\n%s\n", task->date, task->weekday, task->title) >= 0;
    }
    if (fprintf(file, "R %d %d\n%s\n", task->weekday, task->skipped_count, task->title) < 0) {
        return 0;
    }
    for (int i = 0; i < task->skipped_count; i++) {
        if (fprintf(file, "%d\n", task->skipped_dates[i]) < 0) {
            return 0;
        }
    }
    return 1;
}

static int save_tasks(const Task tasks[], int count)
{
    FILE *file = fopen(TEMP_FILE, "w");
    if (file == NULL) {
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < count; i++) {
        if (!write_task_record(file, &tasks[i])) {
            ok = 0;
            break;
        }
    }
    if (fclose(file) != 0) {
        ok = 0;
    }
    if (ok && replace_task_file()) {
        return 1;
    }
    remove(TEMP_FILE);
    return 0;
}

/* 파일에 저장한 뒤 실행 중인 목록에도 변경을 반영한다. */
static int save_changes(Task tasks[], const Task draft[], int count)
{
    if (!save_tasks(draft, count)) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        tasks[i] = draft[i];
    }
    return 1;
}

/* 메뉴 동작 */

static int enter_task(Task *task, int today)
{
    char title[MAX_TITLE + 2];

    for (;;) {
        printf("할 일 입력: ");
        int result = read_line(title, sizeof title);
        if (result == 0) {
            return 0;
        }
        if (result == 1 && title[strspn(title, " \t\r")] != '\0') {
            break;
        }
        printf("빈칸만 입력하거나 %d바이트를 넘길 수 없습니다.\n", MAX_TITLE);
    }

    puts("1. 오늘만   2. 매주 요일 반복   0. 취소");
    int schedule = read_choice("일정 선택: ", 0, 2);
    if (schedule <= 0) {
        return 0;
    }

    int weekday = ONE_TIME;
    if (schedule == 2) {
        puts("1. 월  2. 화  3. 수  4. 목  5. 금  6. 토  7. 일  0. 취소");
        int day = read_choice("반복 요일: ", 0, 7);
        if (day <= 0) {
            return 0;
        }
        weekday = day % 7;
    }

    if (task->weekday != weekday) {
        task->skipped_count = 0;
    }
    strcpy(task->title, title);
    task->date = schedule == 1 ? today : 0;
    task->weekday = weekday;
    return 1;
}

static int append_tasks(Task draft[], int count, int today)
{
    if (count == MAX_TASKS) {
        printf("할 일은 최대 %d개까지 저장할 수 있습니다.\n", MAX_TASKS);
        return -1;
    }

    int next;
    do {
        draft[count] = (Task){0};
        if (!enter_task(&draft[count], today)) {
            return -1;
        }
        count++;

        if (count == MAX_TASKS) {
            puts("1. 저장   0. 취소");
            next = read_choice("선택: ", 0, 1);
        } else {
            puts("1. 저장   2. 할 일 더 입력   0. 취소");
            next = read_choice("선택: ", 0, 2);
        }
    } while (next == 2);

    return next == 1 ? count : -1;
}

static int edit_task(Task draft[], int count, int today)
{
    show_tasks(draft, count, 0, ALL_TASKS);
    if (count == 0) {
        return 0;
    }

    int number = read_choice("수정할 번호 (0. 돌아가기): ", 0, count);
    if (number <= 0 || !enter_task(&draft[number - 1], today)) {
        return 0;
    }
    puts("1. 저장   0. 취소");
    return read_choice("선택: ", 0, 1) == 1;
}

static int add_or_edit(Task tasks[], int count, int today)
{
    puts("\n1. 새 할 일 추가   2. 기존 할 일 수정   0. 돌아가기");
    int action = read_choice("선택: ", 0, 2);
    if (action <= 0) {
        return count;
    }

    Task draft[MAX_TASKS];
    for (int i = 0; i < count; i++) {
        draft[i] = tasks[i];
    }
    int draft_count = count;

    if (action == 1) {
        draft_count = append_tasks(draft, count, today);
    } else if (!edit_task(draft, count, today)) {
        return count;
    }
    if (draft_count < 0) {
        return count;
    }

    if (!save_changes(tasks, draft, draft_count)) {
        puts("저장에 실패했습니다. 실행 폴더의 쓰기 권한을 확인하세요.");
        return count;
    }
    puts("저장했습니다.");
    return draft_count;
}

static void show_weekday(const Task tasks[], int count, struct tm day)
{
    puts("\n1. 월  2. 화  3. 수  4. 목  5. 금  6. 토  7. 일  0. 돌아가기");
    int choice = read_choice("볼 요일: ", 0, 7);
    if (choice <= 0) {
        return;
    }

    int weekday = choice % 7;
    day.tm_mday += (weekday - day.tm_wday + 7) % 7;
    day.tm_isdst = -1;
    if (mktime(&day) == (time_t)-1) {
        puts("선택한 날짜를 계산할 수 없습니다.");
        return;
    }

    printf("\n%s의 할 일 (", weekdays[weekday]);
    print_date(date_number(&day));
    puts(")");
    show_tasks(tasks, count, date_number(&day), weekday);
}

static int choose_reset_date(struct tm *day)
{
    puts("\n1. 오늘 날짜   2. 날짜 직접 입력   0. 돌아가기");
    int choice = read_choice("선택: ", 0, 2);
    if (choice != 2) {
        return choice == 1;
    }

    char input[32];
    const char digits[] = "0123456789";
    for (;;) {
        printf("초기화할 날짜 (YYYY-MM-DD, 빈 줄로 취소): ");
        int result = read_line(input, sizeof input);
        if (result == 0 || (result == 1 && input[0] == '\0')) {
            return 0;
        }

        int year;
        int month;
        int date;
        if (result == 1 && strlen(input) == 10 && input[4] == '-' && input[7] == '-' &&
            strspn(input, digits) == 4 && strspn(input + 5, digits) == 2 &&
            strspn(input + 8, digits) == 2 &&
            sscanf(input, "%4d-%2d-%2d", &year, &month, &date) == 3 &&
            year >= 1 && month >= 1 && month <= 12 && date >= 1 && date <= 31) {
            struct tm selected = {0};
            selected.tm_year = year - 1900;
            selected.tm_mon = month - 1;
            selected.tm_mday = date;
            selected.tm_hour = 12;
            selected.tm_isdst = -1;
            if (mktime(&selected) != (time_t)-1 && selected.tm_year == year - 1900 &&
                selected.tm_mon == month - 1 && selected.tm_mday == date) {
                *day = selected;
                return 1;
            }
        }
        puts("실제 날짜를 YYYY-MM-DD 형식으로 입력하세요.");
    }
}

static int reset_tasks_for_date(Task tasks[], int count, struct tm day)
{
    if (!choose_reset_date(&day)) {
        return count;
    }

    int date = date_number(&day);
    Task draft[MAX_TASKS];
    int draft_count = 0;
    int affected = 0;

    for (int i = 0; i < count; i++) {
        Task task = tasks[i];
        if (task_matches_date(&task, date, day.tm_wday)) {
            affected++;
            if (task.weekday == ONE_TIME) {
                continue;
            }
            if (task.skipped_count == MAX_SKIPPED_DATES) {
                printf("'%s'의 날짜 초기화 기록이 %d개로 가득 찼습니다.\n",
                       task.title, MAX_SKIPPED_DATES);
                return count;
            }
            task.skipped_dates[task.skipped_count] = date;
            task.skipped_count++;
        }
        draft[draft_count] = task;
        draft_count++;
    }

    putchar('\n');
    print_date(date);
    printf(" (%s)의 할 일\n", weekdays[day.tm_wday]);
    show_tasks(tasks, count, date, day.tm_wday);
    if (affected == 0) {
        return count;
    }

    printf("이 날짜의 할 일 %d개를 초기화할까요?\n", affected);
    puts("1. 날짜 초기화   0. 취소");
    if (read_choice("선택: ", 0, 1) != 1) {
        puts("날짜 초기화를 취소했습니다.");
        return count;
    }

    if (!save_changes(tasks, draft, draft_count)) {
        puts("날짜 초기화에 실패했습니다. 실행 폴더의 쓰기 권한을 확인하세요.");
        return count;
    }
    puts("선택한 날짜의 할 일을 초기화했습니다.");
    return draft_count;
}

int main(void)
{
    Task tasks[MAX_TASKS] = {0};
    int count = load_tasks(tasks);
    if (count < 0) {
        fprintf(stderr, "할 일 파일을 읽을 수 없습니다. 파일을 확인하세요.\n");
        return EXIT_FAILURE;
    }

    int alternate_screen = use_alternate_screen();
    for (;;) {
        time_t now = time(NULL);
        struct tm *local = now == (time_t)-1 ? NULL : localtime(&now);
        if (local == NULL) {
            fprintf(stderr, "오늘 날짜를 확인할 수 없습니다.\n");
            return EXIT_FAILURE;
        }
        struct tm today = *local;
        int date = date_number(&today);

        clear_menu_screen(alternate_screen);
        printf("\n오늘의 할 일 (");
        print_date(date);
        printf(", %s)\n", weekdays[today.tm_wday]);
        show_tasks(tasks, count, date, today.tm_wday);

        puts("\n1. 추가(수정)   2. 확인   3. 요일   4. 날짜 초기화   0. 종료");
        int choice = read_choice("선택: ", 0, 4);
        if (choice <= 0) {
            return EXIT_SUCCESS;
        }

        clear_menu_screen(alternate_screen);
        switch (choice) {
            case 1:
                count = add_or_edit(tasks, count, date);
                break;
            case 2:
                puts("\n저장된 할 일");
                show_tasks(tasks, count, 0, ALL_TASKS);
                break;
            case 3:
                show_weekday(tasks, count, today);
                break;
            case 4:
                count = reset_tasks_for_date(tasks, count, today);
                break;
        }
        if (!wait_for_menu(alternate_screen)) {
            return EXIT_SUCCESS;
        }
    }
}
