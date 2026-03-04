#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

int main(){
   while(1){
       cout << "$myshell> ";
       string input;
       if (!getline(cin, input)){
           break;
       }
       if (input == "exit"){
           break;
       }
        if(input.empty()){
            break;
        }
        istringstream ss(input);
        vector<string> tokens;
        string tok;
       while(ss>>tok){
           tokens.push_back(tok);
       }
        if(tokens.empty()){
            continue;
        }
        vector<char*> args;
        for (auto& s : tokens){
            args.push_back(const_cast<char*>(s.c_str()));
        }
        args.push_back(nullptr);
        pid_t pid = fork();
        if(pid<0){
            perror("fork");
            continue;
        }else if(pid==0){
            execvp(args[0], args.data());
            perror("execvp");
            _exit(127);
        }else{
            int status;
            if(waitpid(pid,&status,0)==-1){
                perror("waitpid");
            }
        }
   }
   return 0;
}