#include "Input.h"
#include "Job.h"
#include "Util.h"
#include "Shell.h"
#include <rct/EventLoop.h>
#include <rct/Path.h>
#include <rct/Process.h>
#include <rct/Thread.h>
#include <rct/Log.h>
#include <rct/List.h>
#include <rct/SocketClient.h>
#include <rct/Value.h>
#include <histedit.h>
#include <locale.h>
#include <langinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <sys/select.h>
#include <sys/types.h>
#include <fcntl.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

static std::atomic<int> gotsig;
static bool continuation = false;

static void sig(int i)
{
    gotsig.store(i);
}

static wchar_t* prompt(EditLine* el)
{
    static wchar_t a[] = L"\1\033[7m\1Edit$\1\033[0m\1 ";
    static wchar_t b[] = L"Edit> ";

    return continuation ? b : a;
}

class InputLogOutput : public LogOutput
{
public:
    InputLogOutput(Input* in)
        : LogOutput(0), input(in), inputThreadId(std::this_thread::get_id())
    {}

    virtual void log(const char *msg, int)
    {
        if (inputThreadId == std::this_thread::get_id()) {
            fprintf(stdout, "%s\n", util::utf8ToMB(msg).constData());
        } else {
            input->write(util::utf8ToMB(msg));
        }
    }

private:
    Input* input;
    std::thread::id inputThreadId;
};

unsigned char Input::elComplete(EditLine *el, int)
{
    Input *input = 0;
    el_wget(el, EL_CLIENTDATA, &input);
    assert(input);
    String insert;
    const LineInfoW *lineInfo = el_wline(el);
    const String line = util::wcharToUtf8(lineInfo->buffer);
    const String rest = util::wcharToUtf8(lineInfo->cursor);
    const int len = util::utf8CharacterCount(line);
    const int cursorPos = len - util::utf8CharacterCount(rest);

    const Input::CompletionResult res = input->complete(line, cursorPos, insert);
    if (!insert.isEmpty()) {
        el_winsertstr(el, util::utf8ToWChar(insert).c_str());
    }

    switch (res) {
    case Input::Completion_Refresh: return CC_REFRESH;
    case Input::Completion_Redisplay: return CC_REDISPLAY;
    case Input::Completion_Error: return CC_ERROR;
    }
}

void Input::write(const String& data)
{
    const int r = ::write(mStdoutPipe[1], data.constData(), data.size());
    if (r == -1)
        fprintf(stderr, "Unable to write to input pipe: %d\n", errno);
}

void Input::write(const char* data, ssize_t len)
{
    if (len == -1)
        len = static_cast<ssize_t>(strlen(data));
    int w;
    do {
        w = ::write(mStdoutPipe[1], data, len);
    } while (w == -1 && errno == EINTR);
    if (w == -1)
        fprintf(stderr, "Unable to write to input pipe: %d\n", errno);
}

void Input::sendMessage(Message msg)
{
    const char m = static_cast<char>(msg);
    int w;
    do {
        w = ::write(mMsgPipe[1], &m, 1);
    } while (w == -1 && errno == EINTR);
    if (w == -1)
        fprintf(stderr, "Unable to send message: %d\n", errno);
}

int Input::processFiledescriptors(int mode, wchar_t* ch)
{
    int& readStdout = mStdoutPipe[0];
    int& readMsg = mMsgPipe[0];

    if (readStdout == -1 || readMsg == -1)
        return -1;

    int max = STDIN_FILENO;
    if (readStdout > max)
        max = readStdout;
    if (readMsg > max)
        max = readMsg;

    const bool processStdin = (mode & ProcessStdin);
    assert((processStdin && ch) || (mState == Waiting && !ch));

    fd_set rset;
    char out[4];
    int outpos;
    char buf[8192];
    int r;
    for (;;) {
        FD_ZERO(&rset);
        FD_SET(readStdout, &rset);
        FD_SET(readMsg, &rset);
        FD_SET(STDIN_FILENO, &rset);
        r = ::select(max + 1, &rset, 0, 0, 0);
        if (r <= 0) {
            if (r == -1 && errno == EINTR)
                continue;
            fprintf(stderr, "Select returned %d (%d %s)\n", r, errno, strerror(errno));
            return -1;
        }
        if (FD_ISSET(readStdout, &rset)) {
            for (;;) {
                r = ::read(readStdout, buf, sizeof(buf) - 1);
                if (r > 0) {
                    *(buf + r) = '\0';
                    fprintf(stdout, "%s\n", buf);
                } else {
                    if (!r || (r == -1 && errno != EINTR && errno != EAGAIN)) {
                        ::close(readStdout);
                        readStdout = -1;
                        fprintf (stderr, "Read from stdout pipe returned %d (%d)\n", r, errno);
                        return -1;
                    }
                    if (r == -1 && errno == EAGAIN) {
                        fflush(stdout);
                        if (processStdin && !FD_ISSET(STDIN_FILENO, &rset)) {
                            //printf("refreshing\n");
                            el_wset(mEl, EL_REFRESH);
                        }
                        break;
                    }
                }
            }
        }
        if (FD_ISSET(readMsg, &rset)) {
            int r;
            char msg;
            for (;;) {
                r = ::read(readMsg, &msg, 1);
                if (r == 1) {
                    handleMessage(static_cast<Message>(msg));
                    if (!processStdin && mState == Normal)
                        return 0;
                }
                if (!r || (r == -1 && errno != EINTR && errno != EAGAIN)) {
                    ::close(readMsg);
                    readMsg = -1;
                    fprintf(stderr, "Read from message pipe returned %d (%d)\n", r, errno);
                    return -1;
                }
                if (r == -1 && errno == EAGAIN) {
                    break;
                }
            }
        }
        if (processStdin && FD_ISSET(STDIN_FILENO, &rset)) {
            int r;
            if (isUtf8()) {
                outpos = 0;
                for (;;) {
                    r = ::read(STDIN_FILENO, out + outpos++, 1);
                    if (r <= 0) {
                        fprintf(stderr, "Failed to read (utf8)\n");
                        return -1;
                    }
                    r = mbtowc(ch, out, outpos);
                    if (r > 0) {
                        return 1;
                    }
                    mbtowc(0, 0, 0);
                    if (outpos == 4) {
                        // bad
                        fprintf(stderr, "Invalid utf8 sequence\n");
                        return -1;
                    }
                }
            } else {
                // Go for ascii
                r = ::read(STDIN_FILENO, out, 1);
                if (r <= 0) {
                    fprintf(stderr, "Failed to read (ascii)\n");
                    return -1;
                }
                *ch = out[0];
                return 1;
            }
            fprintf(stderr, "Neither readStdout nor STDIN_FILENO hit\n");
            return -1;
        }
    }
    fprintf(stderr, "Out of getChar\n");
    return -1;
}

int Input::getChar(EditLine *el, wchar_t *ch)
{
    Input *input = 0;
    el_wget(el, EL_CLIENTDATA, &input);
    assert(input);

    return input->processFiledescriptors(ProcessStdin, ch);
}

static void initPipe(int pipe[2])
{
    int flags, ret;
    ret = ::pipe(pipe);
    if (ret != -1) {
        do {
            flags = fcntl(pipe[0], F_GETFL, 0);
        } while (flags == -1 && errno == EINTR);
        if (flags != -1) {
            do {
                ret = fcntl(pipe[0], F_SETFL, flags | O_NONBLOCK);
            } while (ret == -1 && errno == EINTR);
        }
    }
}

static void closePipe(int pipe[2])
{
    if (pipe[0] != -1) {
        ::close(pipe[0]);
        pipe[0] = -1;
    }
    if (pipe[1] != -1) {
        ::close(pipe[1]);
        pipe[1] = -1;
    }
}

void Input::run()
{
    setlocale(LC_ALL, "");
    if (!strcmp(nl_langinfo(CODESET), "UTF-8"))
        mIsUtf8 = true;

    new InputLogOutput(this);

    initPipe(mStdoutPipe);
    initPipe(mMsgPipe);

    (void)signal(SIGINT,  sig);
    (void)signal(SIGQUIT, sig);
    (void)signal(SIGHUP,  sig);
    (void)signal(SIGTERM, sig);

    mEl = nullptr;
    int numc;
    const wchar_t* line;
    HistoryW* hist;
    HistEventW ev;

    hist = history_winit();
    history_w(hist, &ev, H_SETSIZE, 100);
    history_w(hist, &ev, H_LOAD, mOptions.histFile.constData());

    mEl = el_init(mOptions.argv0.constData(), stdin, stdout, stderr);
    el_wset(mEl, EL_CLIENTDATA, this);

    el_wset(mEl, EL_EDITOR, L"emacs");
    el_wset(mEl, EL_SIGNAL, 1);
    el_wset(mEl, EL_GETCFN, &Input::getChar);
    el_wset(mEl, EL_PROMPT_ESC, prompt, '\1');
    for (const auto &rcFile : mOptions.editRcFiles) {
        el_source(mEl, rcFile.constData());
        // ### complain on errors?
    }

    el_wset(mEl, EL_HIST, history_w, hist);

    // complete
    el_wset(mEl, EL_ADDFN, L"ed-complete", L"Complete argument", &Input::elComplete);
    // bind tab
    el_wset(mEl, EL_BIND, L"^I", L"ed-complete", NULL);

    while ((line = el_wgets(mEl, &numc)) && numc) {
        if (int s = gotsig.load()) {
            fprintf(stderr, "got signal %d\n", s);
            gotsig.store(0);
            el_reset(mEl);
        }

        String l = util::wcharToUtf8(line);
        String copy = l;
        copy.chomp(" \t\n");
        if (copy.endsWith('\\')) {
            mBuffer += copy;
            continuation = true;
            history_w(hist, &ev, H_APPEND, line);
            continue;
        }
        history_w(hist, &ev, H_ENTER, line);
        history_w(hist, &ev, H_SAVE, mOptions.histFile.constData());
        continuation = false;
        if (!mBuffer.isEmpty()) {
            l = mBuffer + l;
            mBuffer.clear();
        }

        if (!l.isEmpty()) {
            String err;
            if (l.endsWith('\n'))
                l.chop(1);

            const List<Shell::Token> tokens = tokenize(l, Tokenize_CollapseWhitespace|Tokenize_ExpandEnvironmentVariables, err);
            if (!err.isEmpty()) {
                error() << "Got error" << err;
            } else if (!tokens.isEmpty()) {
                process(tokens);
            }
        }
    }

    history_wend(hist);
    el_end(mEl);

    fprintf(stdout, "\n");

    closePipe(mMsgPipe);
    closePipe(mStdoutPipe);

    EventLoop::mainEventLoop()->quit();
}

static inline const char *findUnescaped(const char *str)
{
    const char ch = *str;
    int escapes = 0;
    while (*++str) {
        if (*str == ch && escapes % 2 == 0) {
            return str;
        }
        if (*str == '\\') {
            ++escapes;
        } else {
            escapes = 0;
        }
    }
    return 0;
}

static inline const char *findEndBrace(const char *str)
{
    // ### need to support comments
    int braces = 1;
    while (*str) {
        switch (*str) {
        case '}':
            if (!--braces)
                return str;
            break;
        case '{':
            ++braces;
            break;
        case '"':
        case '\'':
            str = findUnescaped(str);
            if (!str)
                return 0;
            break;
        }
        ++str;
    }
    return 0;
}

static inline void eatEscapes(String &string)
{
    int i = 0;
    while (i < string.size()) {
        if (string.at(i) == '\\')
            string.remove(i, 1);
        ++i;
    }
}

static inline String stripBraces(String&& string)
{
    String str = std::move(string);
    if (str.isEmpty())
        return str;
    switch (str.first()) {
    case '\'':
    case '"':
    case '{':
        assert(str.size() >= 2);
        return str.mid(1, str.size() - 2);
    }
    return str;
}

void Input::addArg(List<Shell::Token> &tokens, const char *&last, const char *str, unsigned int flags)
{
    if (last && last <= str) {
        tokens.last().args.append(stripBraces(String(last, str - last + 1)));
        tokens.last().raw += " " + String(last, str - last + 1);
        if (flags & Tokenize_CollapseWhitespace) {
            eatEscapes(tokens.last().args.last());
            tokens.last().args.last().chomp(' ');
        }
    }
    last = 0;
}

void Input::addPrev(List<Shell::Token> &tokens, const char *&last, const char *str, unsigned int flags)
{
    if (!tokens.isEmpty() && tokens.last().type == Shell::Token::Command) {
        addArg(tokens, last, str, flags);
        return;
    }
    if (last && last <= str) {
        tokens.append({Shell::Token::Command, stripBraces(String(last, str - last + 1))});
        tokens.last().raw = String(last, str - last + 1);
        if (flags & Tokenize_CollapseWhitespace) {
            eatEscapes(tokens.last().string);
            tokens.last().string.chomp(' ');
        }
    }
    last = 0;
}

List<Shell::Token> Input::tokenize(String line, unsigned int flags, String &err) const
{
    assert(err.isEmpty());
    if (flags & Tokenize_ExpandEnvironmentVariables) {
        int max = 10;
        while (max--) {
            if (!expandEnvironment(line, err)) {
                break;
            }
        }
        if (!err.isEmpty()) {
            return List<Shell::Token>();
        }

        if (max < 0) {
            err = "Too many recursive environment variable expansions";
            return List<Shell::Token>();
        }
    }


    List<Shell::Token> tokens;
    const char *start = line.constData();
    const char *str = start;
    const char *last = str;
    int escapes = 0;
    while (*str) {
        // error() << "checking" << *str;
        // ### isspace needs to be utf8-aware
        if (!last && (!(flags & Tokenize_CollapseWhitespace) || !isspace(static_cast<unsigned char>(*str))))
            last = str;
        if (*str == '\\') {
            ++escapes;
            ++str;
            continue;
        }
        // if (last && str)
        //     printf("last %c %p, str %c %p\n", *last, last, *str, str);

        switch (*str) {
        case '{': {
            addPrev(tokens, last, str - 1, flags);
            const char *end = findEndBrace(str + 1);
            if (!end) {
                err = String::format<128>("Can't find end of curly brace that starts at position %d", str - start);
                return List<Shell::Token>();
            }
            tokens.append({Shell::Token::Javascript, String(str, end - str + 1)});
            str = end;
            break; }
        case '\"':
        case '\'': {
            if (escapes % 2 == 0) {
                const char *end = findUnescaped(str);
                if (end) {
                    str = end;
                } else {
                    err = String::format<128>("Can't find end of quote that starts at position %d", str - start);
                    return List<Shell::Token>();
                }
            }
            break; }
        case '|':
            if (escapes % 2 == 0) {
                addPrev(tokens, last, str - 1, flags);
                if (str[1] == '|') {
                    tokens.append({Shell::Token::Operator, String(str, 2)});
                    ++str;
                } else {
                    tokens.append({Shell::Token::Pipe, String(str, 1)});
                }
            }
            break;
        case '&':
            if (escapes % 2 == 0) {
                addPrev(tokens, last, str - 1, flags);
                if (str[1] == '&') {
                    tokens.append({Shell::Token::Operator, String(str, 2)});
                    ++str;
                } else {
                    tokens.append({Shell::Token::Operator, String(str, 1)});
                }
            }
            break;
        case ';':
        case '<':
        case '>':
        case '(':
        case ')':
        case '!':
            if (escapes % 2 == 0) {
                addPrev(tokens, last, str - 1, flags);
                tokens.append({Shell::Token::Operator, String(str, 1)});
            }
            break;
        case ' ':
            if (escapes % 2 == 0) {
                addPrev(tokens, last, str - 1, flags);
            }
            break;
        default:
            break;
        }
        escapes = 0;
        ++str;
    }
    if (last && last + 1 <= str) {
        if (!tokens.isEmpty() && tokens.last().type == Shell::Token::Command) {
            tokens.last().args.append(stripBraces(String(last, str - last)));
            tokens.last().raw += " " + String(last, str - last + 1);
             if (flags & Tokenize_CollapseWhitespace)
                eatEscapes(tokens.last().args.last());
        } else {
            tokens.append({Shell::Token::Command, stripBraces(String(last, str - last))});
            tokens.last().raw = String(last, str - last);
            if (flags & Tokenize_CollapseWhitespace)
                eatEscapes(tokens.last().string);
        }
    }
    return tokens;
}

void Input::handleMessage(Message msg)
{
    switch (msg) {
    case Resume:
        //el_wset(mEl, EL_REFRESH);
        mState = Normal;
        break;
    }
}

bool Input::tokensAsJavaScript(List<Shell::Token>::const_iterator& token, const List<Shell::Token>::const_iterator& end, String& out)
{
#warning fixme
    return false;
    while (token != end) {
        switch (token->type) {
        case Shell::Token::Command:
            out += token->raw;
            break;
        case Shell::Token::Operator:
            if (token->string == ";") {
                out += ';';
                return true;
            }
            // fall through
        case Shell::Token::Javascript:
            out += token->string;
            break;
        case Shell::Token::Pipe:
            out += ';';
            return true;
        }
        ++token;
    }
    if (!out.isEmpty()) {
        out += ';';
        return true;
    }
    return false;
}

// static inline ChainJavaScript *createJS(const String &script)
// {
//     if (NodeJS::SharedPtr nodeJS = Shell::instance()->nodeJS()) {
//         //printf("trying js: %s\n", jsscript.constData());
//         if (nodeJS->checkSyntax(script)) {
//             ChainJavaScript *js = new ChainJavaScript(script);
//             return js;
//         }
//     }
//     return 0;
// }

static inline int createNodeJS(const String& socketFile)
{
    SocketClient::SharedPtr client = std::make_shared<SocketClient>(SocketClient::Blocking);
    if (!client->connect(socketFile)) {
        return -1;
    }
    return client->takeFD();
}

void Input::processTokens(const List<Shell::Token>& tokens)
{
    const String path = Shell::instance()->environment("PATH");

    Job::SharedPtr job = std::make_shared<Job>(STDOUT_FILENO);
    auto token = tokens.cbegin();
    const auto end = tokens.cend();
    while (token != end) {
        assert(!token->string.isEmpty());

        //printf("token %s\n", Shell::Token::typeName(token->type));
        bool error = false;

        switch (token->type) {
        case Shell::Token::Command: {
            bool isCommand = false;
            // If the file explicitly exists, run it
            if (token->string[0] == '.' || token->string[0] == '/') {
                const Path file = token->string;
                if (file.exists())
                    isCommand = true;
            }

            // give JS a chance
            if (!isCommand) {
                String jsscript;
                if (tokensAsJavaScript(token, end, jsscript)) {
#warning fixme
                    //c = createJS(jsscript);
                } else {
                    isCommand = true;
                }
            }

            if (isCommand) {
                // see if we can find a command by this name
                const Path file = util::findFile(path, token->string);
                //error() << "running command" << file << token->args;
                if (!file.isEmpty()) {
                    if (!job->addProcess(file, token->args, List<String>(), (token + 1 == end)))
                        error = true;
                } else {
                    error = true;
                }
            }
            if (error) {
                printf("Invalid command: %s\n", token->string.constData());
                error = true;
            }
            break; }
        case Shell::Token::Javascript: {
            const int fd = createNodeJS(mOptions.socketFile);
            if (fd == -1) {
                printf("Unable to open connection to node\n");
                error = true;
            } else {
                //printf("adding connection %d ('%s')\n", fd, token->string.constData());
                job->addNodeJS(token->string, fd, (token + 1 == end));
            }
            break; }
        case Shell::Token::Operator:
            job->wait();
            job = std::make_shared<Job>(STDOUT_FILENO);
            break;
        case Shell::Token::Pipe:
            break;
        }
        if (error) {
            job = std::make_shared<Job>(STDOUT_FILENO);
        }
        ++token;
    }

    job->wait();
}

void Input::process(const List<Shell::Token> &tokens)
{
    for (const auto& token : tokens) {
        String args;
        for (const String& arg : token.args) {
            args += arg + ", ";
        }
        if (!args.isEmpty())
            args.chop(2);
        error() << String::format<128>("[%s] %s (%s)", token.string.constData(), Shell::Token::typeName(token.type), args.constData());
    }
    // return;

    processTokens(tokens);
}

enum EnvironmentCharFlag {
    Invalid,
    Valid,
    ValidNonStart
};

static inline bool environmentVarChar(unsigned char ch)
{
    if (isdigit(ch))
        return ValidNonStart;
    return (isalpha(ch) || ch == '_' ? Valid : Invalid);
}

bool Input::expandEnvironment(String &string, String &err) const
{
    Hash<String, String> environ = Shell::instance()->environment();

    int escapes = 0;
    for (int i=0; i<string.size() - 1; ++i) {
        switch (string.at(i)) {
        case '$':
            if (escapes % 2 == 0) {
                if (string.at(i + 1) == '{') {
                    for (int j=i + 2; j<string.size(); ++j) {
                        if (string.at(j) == '}') {
                            const String env = string.mid(i + 2, j - (i + 2));
                            const String sub = environ.value(env);
                            string.replace(i, j - i + 1, sub);
                            // error() << "Got sub" << string.mid(i + 2, j - 2);
                        } else if (environmentVarChar(string.at(j)) == Invalid) {
                            err = "Bad substitution";
                            return false;
                        }
                    }

                } else if (string.at(i + 1) == '$') {
                    string.remove(i + 1, 1);
                } else if (environmentVarChar(string.at(i + 1)) == Valid) {
                    int j=i + 2;
                    while (j<string.size() && environmentVarChar(string.at(j)) != Invalid)
                        ++j;
                    const String sub = environ.value(string.mid(i + 1, j - 1));
                    string.replace(i, j - i + 1, sub);
                } else {
                    err = "Bad substitution";
                    return false;
                }
                // if (
                // if (isstring.at(i + 1)
            }
            escapes = 0;
            break;
        case '\\':
            ++escapes;
            break;
        default:
            escapes = 0;
            break;
        }
    }
}

// static Value jsCall(const String &object, const String &function,
//                     const List<Value> &args, bool *ok = 0)
// {
//     Shell* shell = Shell::instance();
//     return shell->runAndWait<Value>([&]() -> Value {
//             return shell->nodeJS()->call(object, function, args, ok);
//         });
// }

// static Value jsCall(const String &function, const List<Value> &args, bool *ok = 0)
// {
//     return jsCall(String(), function, args, ok);
// }

// cursor is the unicode character position, line is in utf8
Input::CompletionResult Input::complete(const String &line, int cursor, String &insert)
{
    // String err;
    // const List<Shell::Token> tokens = tokenize(line, Tokenize_None, err);
    // if (!err.isEmpty()) {
    //     return Completion_Error;
    // } else if (tokens.isEmpty()) {
    //     return Completion_Refresh;
    // }

    // List<Value> args;
    // args.append(line);
    // args.append(cursor);
    // List<Value> toks;
    // for (auto token : tokens) {
    //     Map<String, Value> val;
    //     val["type"] = Shell::Token::typeName(token.type);
    //     val["string"] = token.string;
    //     toks.append(val);
    // }
    // args.append(toks);
    // bool ok;
    // //const Value value = mNodeJS->call("complete", args, &ok);
    // const Value value = jsCall("complete", args, &ok);
    // if (!ok)
    //     return Completion_Error;
    // const String result = value.value<String>("result");
    // insert = value.value<String>("insert");
    // if (result == "refresh") {
    //     return Completion_Refresh;
    // } else if (result == "redisplay") {
    //     return Completion_Redisplay;
    // } else {
    //     insert.clear();
    //     return Completion_Error;
    // }
}
void Input::launchNode()
{
    assert(!mNodeProcess);
    mNodeProcess = new Process;
    List<String> args;
    args << JSH_DOT_JS << String::format<128>("--socket-file=%s", mOptions.socketFile.constData());
    mNodeProcess->readyReadStdErr().connect([](Process *proc) {
            error() << "nodejs stderr" << proc->readAllStdErr();
        });

    if (!mNodeProcess->start("node", args) && !mNodeProcess->start("nodejs", args)) {
        error("Can't launch nodejs %s", mNodeProcess->errorString().constData());
        delete mNodeProcess;
        mNodeProcess = 0;
    } else {
        mNodeProcess->closeStdIn();
        mNodePingTimer.restart(30000); // ### configurable?
    }
}

void Input::pingNode(Timer *)
{
    if (mNodeFD == -1) {
        mNodeFD = createNodeJS(mOptions.socketFile);
        if (mNodeFD == -1)
            return;
    }
    NodeConnection connection(mNodeFD);
    if (!connection.send() || !connection.send(String()) {
        fprintf(stderr, "unable to write script to node (%d)\n", script.size());
        return false;
    }

}