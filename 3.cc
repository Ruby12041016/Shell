#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <linux/limits.h>

using namespace std;

struct Commond {
    vector<string> args;
    string input_file;
    string output_file;
    string error_file;
    bool append;
    bool background;
    bool pipe_out;
    bool input;
    bool output;
    bool error;
};

void init_com(Commond& com, bool background) {
    com.args = {};
    com.input_file = "";
    com.output_file = "";
    com.error_file = "";
    com.append = false;
    com.background = background;
    com.pipe_out = false;
    com.input = com.output = com.error = false;
}

void delete_tok(string& token) {
    size_t start = token.find_first_not_of(" \t\n");
    size_t end = token.find_last_not_of(" \t\n");
    if (start == string::npos) {
        return;
    }
    token = token.substr(start, end - start + 1);
}

vector<Commond> parse_cmd(string& cmd, bool& redirect) {
    delete_tok(cmd);
    vector<Commond> cmds;
    vector<string> tokens;
    string token;
    istringstream ss(cmd);
    while (ss >> token) {
        tokens.push_back(token);
    }
    if (tokens.empty())
        return cmds;
    bool backgr = false;
    if (tokens.back() == "&") {
        backgr = true;
        tokens.pop_back();
    }
    vector<vector<string>> pipe_cmd;
    vector<string> now_cmd;
    redirect = false;
    for (auto& toke : tokens) {
        if (toke == ">" || toke == "<" || toke == ">>" || toke == "2>") {
            redirect = true;
        }
        if (toke == "|") {
            if (!now_cmd.empty()) {
                pipe_cmd.push_back(now_cmd);
                now_cmd.clear();
            } else {
                cerr << "无效管道！" << endl;
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
        init_com(com, backgr);
        com.pipe_out = (i < pipe_cmd.size() - 1);
        for (size_t j = 0; j < pipe_cmd[i].size(); j++) {
            string& tok = pipe_cmd[i][j];
            if (tok == ">") {
                if (j + 1 < pipe_cmd[i].size()) {
                    com.output_file = pipe_cmd[i][++j];
                    com.output = true;
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
        cmds.push_back(com);
    }
    return cmds;
}

void do_redirect(Commond& cmds) {
    if (cmds.input) {
        int fd = open(cmds.input_file.c_str(), O_RDONLY, 0644);
        if (fd < 0) {
            perror("open");
            exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (cmds.output && cmds.append) {
        int fd =
            open(cmds.output_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("open");
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    if (cmds.output && !cmds.append) {
        int fd =
            open(cmds.output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open");
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    if (cmds.error) {
        int fd =
            open(cmds.error_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open");
            exit(1);
        }
        dup2(fd, 2);
        close(fd);
    }
}

void do_pipe(vector<Commond>& cmds) {
    size_t n = cmds.size();
    int pre_read_fd = STDIN_FILENO;
    int curr_pipe[2];
    for (size_t i = 0; i < n; i++) {
        if (i < n - 1) {
            if (pipe(curr_pipe) < 0) {
                perror("pipe failed");
                exit(1);
            }
        }
        pid_t pid = fork();
        if (pid <= -1) {
            perror("fork failed");
            exit(-1);
        }
        if (pid == 0) {
            if (pre_read_fd != STDIN_FILENO) {
                dup2(pre_read_fd, STDIN_FILENO);
                close(pre_read_fd);
            }
            if (i < n - 1) {
                close(curr_pipe[0]);
                dup2(curr_pipe[1], STDOUT_FILENO);
                close(curr_pipe[1]);
            }
            do_redirect(cmds[i]);
            vector<string> cmmond = cmds[i].args;
            vector<char*> argvs;
            for (auto& s : cmmond) {
                argvs.push_back(const_cast<char*>(s.c_str()));
            }
            argvs.push_back(nullptr);
            execvp(argvs[0], argvs.data());
            perror("exec");
            exit(1);
        } else {
            if (pre_read_fd != STDIN_FILENO) {
                close(pre_read_fd);
            }
            if (i < n - 1) {
                close(curr_pipe[1]);
                pre_read_fd = curr_pipe[0];
            } else {
                pre_read_fd = STDIN_FILENO;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        wait(NULL);
    }
}

char* old_pwd = nullptr;

void do_cd(vector<char*> argv) {
    char* dir = NULL;
    char now_pwd[PATH_MAX];
    static string new_dir;
    if (getcwd(now_pwd, sizeof(now_pwd)) == NULL) {
        perror("getcwd");
        return;
    }
    // if(old_pwd){
    //     free(old_pwd);
    // }
    // old_pwd = strdup(now_pwd);
    // old_pwd = now_pwd;
    //  setenv("OLDPWD", old_pwd, 1);
    if (argv[1] == NULL || !strcmp(argv[1], "~")) {
        dir=getenv("HOME");
        if(dir==NULL){
            cerr << "HOME NOT SET" << endl;
            return;
        }
    }
    else if (!strcmp(argv[1], "-")) {
        if (old_pwd == NULL) {
            cerr<<"OLD_PWD NOT SET"<<endl;
            return;
        }
        dir = old_pwd;
        cout << dir << endl;
    }
    else {
        char* n_dir = argv[1];
        if(n_dir[0]=='~'){
            if(n_dir[1]=='\0'||n_dir[1]=='/'){
                char* home = getenv("HOME");
                if(home){
                    new_dir = string(home) + (n_dir + 1);
                    dir = const_cast<char*>(new_dir.c_str());
                }else{
                    dir = n_dir;
                }
            }else{
                dir = n_dir;
            }
        }else{
            dir = n_dir;
        }
    }
    if(dir==NULL){
        cerr << "DIR NOT SET" << endl;
        return;
    }
    if (chdir(dir) == -1) {
        perror("cd failed");
        return;
    }
    if (old_pwd) {
        free(old_pwd);
    }
    old_pwd = strdup(now_pwd);
    setenv("OLDPWD", old_pwd, 1);
    if (getcwd(now_pwd, sizeof(now_pwd)) != NULL) {
        setenv("PWD", now_pwd, 1);
    }
}

void build_in(Commond& cmds) {
    if (cmds.args.empty()) {
        return;
    }
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
}

int main() {
    while (1) {
        cout << "$myshell> ";
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
        vector<Commond> commond = parse_cmd(input, redirect);
        if(commond.empty()||commond[0].args.empty()){
            continue;
        }
        vector<char*> argvs;
        for (auto& s : commond[0].args) {
            argvs.push_back(const_cast<char*>(s.c_str()));
        }
        if (!strcmp(argvs[0], "cd") || !strcmp(argvs[0], "pwd")) {
            build_in(commond[0]);
        }else{
            do_pipe(commond);
        }
    }
    return 0;
}