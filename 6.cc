#include <fcntl.h>
#include <linux/limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

struct Commond {
    vector<string> args; //命令字符串
    string input_file;  //输入文件，用于重定向
    string output_file; //输出文件，用于重定向
    string error_file; //错误文件，用于重定向
    bool append;    //是否追加重定向
    bool background; //是否后台运行
    bool input; //是否输入重定向
    bool output; //是否输出重定向
    bool error; //是否错误重定向
};

vector<pid_t> bg_pid;  //后台运行pid数组
int bg_count=0;    //后台运行id
bool is_bg = false;  //是否后台运行
void init_com(Commond& com) {
    com.args = {};
    com.input_file = "";
    com.output_file = "";
    com.error_file = "";                //初始化命令结构体
    com.append = false;
   // com.background = background;
    com.input = com.output = com.error = false;
}

void delete_tok(string& token) {
    size_t start = token.find_first_not_of(" \t\n");
    size_t end = token.find_last_not_of(" \t\n");           //删除输入字符串两端的空白字符
    if (start == string::npos) {
        return;
    }
    token = token.substr(start, end - start + 1);
}

vector<Commond> parse_cmd(string& cmd, bool& redirect) {   //解析输入的字符串
    delete_tok(cmd);
    vector<Commond> cmds;
    vector<string> tokens;
    string token;
    istringstream ss(cmd);
    while (ss >> token) {
        tokens.push_back(token);
        if (token == "ls") {  //如果输入了ls命令（这里的ls一定是独立的，因为是按空格切割），插入参数--color
            tokens.push_back("--color");
        }
    }
    if (tokens.empty())  //如果没有输入，返回空命令
        return cmds;
   // bool backgr = false;
    if (tokens.back() == "&") {
       // backgr = true;        //检测是否后台运行
       is_bg = true;
       tokens.pop_back();
    }
    vector<vector<string>> pipe_cmd;  //拆分各个管道命令并储存
    vector<string> now_cmd;
    redirect = false;   
    for (auto& toke : tokens) {     //检测是否有重定向
        if (toke == ">" || toke == "<" || toke == ">>" || toke == "2>") {
            redirect = true;
        }
        if (toke == "|") {
            if (!now_cmd.empty()) {  
                pipe_cmd.push_back(now_cmd);
                now_cmd.clear();
            } else {
                cerr << "\033[31m" << "无效管道！" << "\033[37m" << endl;
                return cmds;
            }
        } else {
            now_cmd.push_back(toke);
        }
    }
    if (!now_cmd.empty()) {
        pipe_cmd.push_back(now_cmd);
    }
    for (size_t i = 0; i < pipe_cmd.size(); i++) {   
        Commond com;
        init_com(com);
        for (size_t j = 0; j < pipe_cmd[i].size(); j++) {    //解析每个管道命令
            string& tok = pipe_cmd[i][j];
            if (tok == ">") {
                if (j + 1 < pipe_cmd[i].size()) {
                    com.output_file = pipe_cmd[i][++j];   //如果有重定向符号，标记并设置相关输入输出文件，方便执行重定向
                    com.output = true;                    //j+1......确保不会越界访问                  
                }
            } else if (tok == "<") {
                if (j + 1 < pipe_cmd[i].size()) {
                    com.input_file = pipe_cmd[i][++j];
                    com.input = true;
                }
            } else if (tok == ">>") {
                if (j + 1 < pipe_cmd[i].size()) {
                    com.output_file = pipe_cmd[i][++j];
                    com.output = true;
                    com.append = true;
                }
            } else if (tok == "2>") {
                if (j + 1 < pipe_cmd[i].size()) {
                    com.error_file = pipe_cmd[i][++j];
                    com.error = true;
                }
            } else {
                com.args.push_back(tok);    
            }
        }
        cmds.push_back(com);  //储存各管道命令
    }
    return cmds;
}

void do_redirect(Commond& cmds) {  //执行重定向
    if (cmds.input) {    //如果有输入重定向，把标准输入描述符重定向到标记的输入文件的文件描述符，以此类推
        int fd = open(cmds.input_file.c_str(), O_RDONLY , 0644);
        dup2(fd, 0);
        close(fd);
    }
    if (cmds.output && cmds.append) {
        int fd =
            open(cmds.output_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        dup2(fd, 1);
        close(fd);
    }
    if (cmds.output && !cmds.append) {
        int fd =
            open(cmds.output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 1);
        close(fd);
    }
    if (cmds.error) {
        int fd =
            open(cmds.error_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 2);
        close(fd);
    }
}

void do_pipe(vector<Commond>& cmds) { //执行管道命令和各外置命令
    size_t n = cmds.size();
    int pre_read_fd = STDIN_FILENO;  //储存上一个命令的输出端
    int curr_pipe[2];  
    vector<pid_t> pids;  //储存各子进程的进程id
    for (size_t i = 0; i < n; i++) {   //创建管道
        if (i < n - 1) {
            if (pipe(curr_pipe) < 0) {
                perror("pipe failed");
                exit(1);
            }
        }
        pid_t pid = fork();   //为各管道命令创建子进程
        pids.push_back(pid);  
        if (pid == 0) {
            // 子进程恢复默认信号处理
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            if (pre_read_fd != STDIN_FILENO) {    //如果上一个管道不是键盘，将输入端重定向到上一个管道的输出端
                dup2(pre_read_fd, STDIN_FILENO);
                close(pre_read_fd);      //关闭不用的输出管道描述符
            }
            if (i < n - 1) {    //如果不是最后一个管道
                close(curr_pipe[0]);      //关闭当前管道的输入描述符（子进程不用）
                dup2(curr_pipe[1],STDOUT_FILENO);  // 将标准输出重定向到当前管道的输出端
                close(curr_pipe[1]);    
            }
            do_redirect(cmds[i]);   //执行重定向（有才会执行）
            vector<string> cmmond = cmds[i].args;  
            vector<char*> argvs;
            for (auto& s : cmmond) {
                argvs.push_back(const_cast<char*>(s.c_str()));  //构建命令数组
            }
            argvs.push_back(nullptr);   
            execvp(argvs[0], argvs.data());  //执行命令
            perror("exec");
            exit(1);
        } else {   //父进程关闭没用管道描述符
            if (pre_read_fd != STDIN_FILENO) {
                close(pre_read_fd);   
            }
            if (i < n - 1) {
                close(curr_pipe[1]);
                pre_read_fd = curr_pipe[0];  //更新为当前命令的输入端（不是最后一个命令时）
            } else {
                pre_read_fd = STDIN_FILENO;
            }
             
        }   
    }
    if (is_bg) {    //如果后台运行，不等待
            for (size_t i = 0; i < pids.size(); i++){
                    bg_pid.push_back(pids[i]);
                    bg_count++;
                    int id = bg_count;
                    cout << "[" << id << "]" << "  后台运行中" << endl;
            }
                sleep(10);    //等待后台运行，防止阻塞
    } else {
        for (size_t i = 0; i < pids.size(); i++) {    //不是后台运行就分别等待
            waitpid(pids[i], NULL, 0);
        }
    }
}

char* old_pwd = nullptr;  //记录上一个路径

void do_cd(vector<char*> argv) {    //cd和pwd是内置命令
    char* dir = NULL;
    char now_pwd[PATH_MAX];
    static string new_dir;
    if (getcwd(now_pwd, sizeof(now_pwd)) == NULL) {  //获取当前路径
        perror("getcwd");
        return;
    }
    if (argv[1] == NULL || !strcmp(argv[1], "~")) {  //实现cd ~
        dir = getenv("HOME");
        if (dir == NULL) {
            cerr << "\033[31m" << "HOME NOT SET" << "\033[37m" << endl;
            return;
        }
    } else if (!strcmp(argv[1], "-")) {     //实现cd -
        if (old_pwd == NULL) {
            cerr << "\033[31m" << "OLD_PWD NOT SET" << "\033[37m" << endl;
            return;
        }
        dir = old_pwd;
        cout << dir << endl;
    } else {
        char* n_dir = argv[1];
        if (n_dir[0] == '~') {  //如果是以~开头的路径名，构建完整路径
            if (n_dir[1] == '\0' || n_dir[1] == '/') {
                char* home = getenv("HOME");
                if (home) {
                    new_dir = string(home) + (n_dir + 1);
                    dir = const_cast<char*>(new_dir.c_str());
                } 
            } else {
                dir = n_dir;
            }
        } else {
            dir = n_dir;
        }
    }
    if (dir == NULL) {
        cerr << "\033[31m" << "DIR NOT SET" << "\033[37m" << endl;
        return;
    }
    if (chdir(dir) == -1) {    //切换路径
        return;
    }
    if (old_pwd) {   
        free(old_pwd);   //释放内存
    }
    old_pwd = strdup(now_pwd);   //保存当前路径
    setenv("OLDPWD", old_pwd, 1);   //设置环境变量
    if (getcwd(now_pwd, sizeof(now_pwd)) != NULL) {
        setenv("PWD", now_pwd, 1);
    }
}

void build_in(Commond& cmds) {    //执行内置命令
    if (cmds.args.empty()) {
        return;
    }
    int saved_STDIN = -1;    //用来判断是否执行了重定向
    int saved_STDOUT = -1;
    int saved_STDERR = -1;
    if (cmds.input) {
        saved_STDIN = dup(STDIN_FILENO);     //获取相应的标准描述符，方便后面重定向回去，因为内置命令没有创建子进程，不重定向回去会
    }                                       //因为内置命令没有创建子进程，不重定向回去会影响后面的命令
    if (cmds.output) {
        saved_STDOUT = dup(STDOUT_FILENO);
    }
    if (cmds.error) {
        saved_STDERR = dup(STDERR_FILENO);
    }
    do_redirect(cmds);    //执行重定向
    vector<char*> argvs;
    for (auto& s : cmds.args) {
        argvs.push_back(const_cast<char*>(s.c_str()));
    }
        argvs.push_back(nullptr);
        if (!strcmp(argvs[0], "cd")) {
            do_cd(argvs);
        } else if (!strcmp(argvs[0], "pwd")) {
            char now_pwd[PATH_MAX];
            if (getcwd(now_pwd, sizeof(now_pwd)) == NULL) {
                perror("getcwd");
            }
            cout << now_pwd << endl;
        }
    
    if (saved_STDIN != -1) {
        dup2(saved_STDIN, STDIN_FILENO);
        close(saved_STDIN);
    }
    if (saved_STDOUT != -1) {                          //重定向回标准输入输出
        dup2(saved_STDOUT, STDOUT_FILENO);
        close(saved_STDOUT);
    }
    if (saved_STDERR != -1) {
        dup2(saved_STDERR, STDERR_FILENO);
        close(saved_STDERR);
    }
}

string preprocess(string& input) {            //处理重定向符号前后没有空格的情况
    string result;
    for (size_t i = 0; i < input.size(); i++) {
        char a, b;
        a = input[i];
        if (i + 1 < input.size()) {
            b = input[i + 1];
        } else {
            b = '\0';
        }
        if ((a == '2' && b == '>') || (a == '>' && b == '>')) {
            if (i > 0 && input[i - 1] != ' ') {
                result += ' ';
            }
            result += a;
            result += b;
            if (i + 1 < input.size() && input[i + 2] != ' ') { 
                result += ' ';
            }
            i++;    //跳过b
        } else if (a == '>' || a == '<') {
            if (i > 0 && input[i - 1] != ' ') {
                result += ' ';
            }
            result += a;
            if (i + 1 < input.size() && input[i + 1] != ' ') {
                result += ' ';
            }
        } else {
            result += a;
        }
    }
    return result;
}

void sign_hander(int sig){   //信号处理函数，回收后台进程，防止出现僵尸进程
    int status;
    pid_t pid;
    while((pid=waitpid(-1,&status,WNOHANG))>0){      //回收执行完的后台进程
        cout << "[" << bg_count << "]" << " DONE     "<<"PID: "<<pid << endl;
        for (int i = 0; i < bg_count; i++) {
            if(bg_pid[i]==pid){
                for (int j = i; j < bg_count-1;j++){
                    bg_pid[j] = bg_pid[j + 1];
                }
                bg_pid.pop_back();
                bg_count--;
                break;
            }
        }
    }
}

int main() {
    chdir(getenv("HOME"));

    signal(SIGTTIN, SIG_IGN);   //防止后台进程读操作
    signal(SIGTTOU, SIG_IGN);   //防止后台进程写操作
    signal(SIGINT, SIG_IGN);   // shell忽略Ctrl+C
    signal(SIGTSTP, SIG_IGN);  // shell忽略Ctrl+Z
    signal(SIGCHLD, sign_hander);  //处理后台进程运行结束信号

    while (1) {
        is_bg = false;   
        char h_path[PATH_MAX];
        getcwd(h_path, sizeof(h_path));
        string home = getenv("HOME");
        string path(h_path);
        if (path.find(home) == 0) {
            path = "~" + path.substr(home.size());
        }
        cout << "\e[32m" << "\e[1m" << "->  " << "\e[36m" << path << " $ "
             << "\e[37m" << "\e[0m";
        string input;
        if (!getline(cin, input)) {
            break;
        }
        if (input == "exit") {
            break;
        }
        if (input.empty()) {
            break;
        }
        bool redirect;
        string preprocessed = preprocess(input);
        vector<Commond> commond = parse_cmd(preprocessed, redirect);
        if (commond.empty() || commond[0].args.empty()) {
            continue;
        }
        if (commond.size() > 1) {
            string first_cmd = commond[0].args[0];
            if (first_cmd == "cd" || first_cmd == "pwd") {
                cerr << "\e[31m" << "内置命令不支持管道" << "\e[37m" << endl;
                continue;
            }
        }
        vector<char*> argvs;
        for (auto& s : commond[0].args) {
            argvs.push_back(const_cast<char*>(s.c_str()));
        }
        if (!strcmp(argvs[0], "cd") || !strcmp(argvs[0], "pwd")) {
            build_in(commond[0]);
        } else {
            do_pipe(commond);
        }
    }
    if (old_pwd) {
        free(old_pwd);
        old_pwd = nullptr;
    }
    return 0;
}