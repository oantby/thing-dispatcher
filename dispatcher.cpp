/*
dispatcher

Serves as something of a nohup, something of a sudo, etc. Takes requests from
other local processes to kick off known, registered processes. Primary use
at time of creation is to allow apache to kick off long-running processes
without using something like launch daemons watching file triggers.
*/

#include <iostream>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <exception>
#include <map>
#include <fstream>
#include <vector>

using namespace std;

#define SOCKET_DEFAULT "/tmp/stuff_dispatcher.sock"
#define LOCKFILE_DEFAULT "/tmp/stuff_dispatcher.lock"

int Main_socket;

struct {
	string commands_path;
	string lockfile_path;
	string socket_path;
} Ap;

void rm_socket() {
	if (Main_socket) close(Main_socket);
	unlink(Ap.socket_path.c_str());
	unlink(Ap.lockfile_path.c_str());
}

int get_socket() {
	
	int lock = open(Ap.lockfile_path.c_str(), O_WRONLY | O_CREAT);
	if (lock == -1) {
		cerr << "Failed to open lockfile: " << strerror(errno) << endl;
		exit(1);
	}
	struct flock f;
	
	f.l_type = F_WRLCK;
	f.l_whence = SEEK_SET;
	f.l_start = f.l_len = 0;
	
	if (fcntl(lock, F_SETLK, &f) == -1) {
		if (errno == EAGAIN || errno == EACCES) {
			cerr << "Existing process has lock on LOCKFILE. Exiting" << endl;
			exit(0);
		} else {
			cerr << "Failed to lock lockfile: " << strerror(errno) << endl;
			exit(1);
		}
	}
	
	struct stat st;
	memset(&st, 0, sizeof(st));
	if (stat(Ap.socket_path.c_str(), &st) != -1) {
		// we managed to take the lockfile, but socket already exists.
		// we need to blow it away.
		unlink(Ap.socket_path.c_str());
	}
	
	int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (sock == -1) {
		cerr << "Failed to get socket: " << strerror(errno) << endl;
		exit(1);
	}
	struct sockaddr_un name;
	memset(&name, 0, sizeof(name));
	name.sun_family = AF_UNIX;
	strcpy(name.sun_path, Ap.socket_path.c_str());
	
	int r = bind(sock, (const struct sockaddr *) &name, sizeof(name));
	if (r == -1) {
		cerr << "Failed to bind socket to filesystem: " << strerror(errno) << endl;
		exit(1);
	}
	
	// need to make sure anyone can write to us. Presume we own the
	// socket file and set its permissions.
	chmod(Ap.socket_path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
	
	atexit(rm_socket);
	
	return sock;
}

void usage(char *name) {
	cout << "Usage: " << name << " -c commands_path\nCommands path contains "
		"tab-separated command name - program invocation pairs" << endl;
	exit(1);
}

void parse_args(int argc, char **argv) {
	int c;
	
	// set defaults
	Ap.socket_path = SOCKET_DEFAULT;
	Ap.lockfile_path = LOCKFILE_DEFAULT;
	
	extern char *optarg;
	while ((c = getopt(argc, argv, "c:s:l:h")) != EOF) {
		switch (c) {
			case 'c':
				Ap.commands_path = optarg;
				break;
			case 's':
				Ap.socket_path = optarg;
				break;
			case 'l':
				Ap.lockfile_path = optarg;
				break;
			default:
				usage(*argv);
				break;
		}
	}
	
	if (!Ap.commands_path.size()) usage(*argv);
}

// returns the invocation string for the given command.
string find_command(const string &cmd) {
	static time_t last_update = 0;
	static map<string, string> cmds;
	
	time_t now = time(NULL);
	
	if (!cmds.count(cmd) && last_update + 5 > now) {
		// updated very recently, and command not present.
		throw invalid_argument("Requested command not found");
	}
	
	if (now > last_update + 30) {
		cerr << "Updating commands list\n";
		// even if we found this command, if it's TOO old, we want to reload
		// the config, to prevent running old commands.
		ifstream ifile(Ap.commands_path);
		bool replace = ifile.good();
		string comm;
		map<string, string> newMap;
		while (getline(ifile, comm)) {
			// skip comments and empties.
			if (comm.size() == 0 || comm[0] == '#') continue;
			size_t pos = comm.find('\t');
			if (pos == string::npos) {
				newMap[comm] = ""; // noop
				cerr << "Found command (noop): " << comm << '\n';
			} else {
				newMap[comm.substr(0, pos)] = comm.substr(pos + 1);
				cerr << "Found command: " << comm.substr(0, pos) << ": " << comm.substr(pos + 1) << '\n';
			}
		}
		ifile.close();
		if (replace) {
			cmds = newMap;
			last_update = time(NULL);
		} else {
			cerr << "Warn: Failed to update from config file" << endl;
		}
		cerr << flush;
	}
	
	if (cmds.count(cmd)) {
		return cmds[cmd];
	}
	cerr << "Search [" << cmd << "] no match" << endl;
	throw invalid_argument("Requested command not found");
}

void run_command(const string &cmd) {
	if (!cmd.size()) return;
	int r;
	r = fork();
	if (r > 0) {
		// parent. we're done.
		return;
	} else if (r == -1) {
		// parent, but we got an error.
		throw runtime_error("Failed to fork new process");
	}
	
	// reset signal handlers
	struct sigaction sa;
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGCHLD, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	
	// close all file descriptors (that we know and care about).
	close(Main_socket);
	close(0);
	close(1);
	close(2);
	
	setsid();
	r = fork();
	if (r > 0) {
		_exit(0);
	} else if (r == -1) {
		// second fork failed. we've got no way to report that though...
		// I guess we just exit.
		_exit(1);
	}
	// fully clean session now.
	
	const char **args, *name;
	size_t pos = 0, pos2 = 0;
	vector<string> words;
	while ((pos2 = cmd.find(' ', pos)) != string::npos) {
		words.push_back(cmd.substr(pos, pos2 - pos));
		pos = pos2 + 1;
	}
	words.push_back(cmd.substr(pos));
	args = (const char **)calloc(words.size() + 1, sizeof(char *));
	name = &words[0][0];
	for (size_t i = 0; i < words.size(); i++) {
		args[i] = &words[i][0];
	}
	// child. we exec and we're happy.
	r = execv(name, (char * const *)args);
	if (r == -1) {
		cerr << "Failed to execv: " << strerror(errno) << endl;
		// cannot return to calling function. must exit here.
		// _exit so we don't run atexit and remove our socket.
		_exit(1);
	}
}

void interrupt(int sig) {
	// use exit() to ensure atexit functions exit.
	exit(128 + sig);
}

void send_resp(struct sockaddr_un *addr, const char *msg) {
	if (sendto(Main_socket, msg, strlen(msg), 0, (struct sockaddr *)addr,
		sizeof(struct sockaddr_un)) == -1) {
		cerr << "Failed to send response: " << strerror(errno) << endl;
	}
}

int main(int argc, char **argv) {
	parse_args(argc, argv);
	Main_socket = get_socket();
	
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGCHLD, &sa, 0) == -1) {
		cerr << "Failed to ignore SIGCHLD: " << strerror(errno) << endl;
		exit(1);
	}
	sa.sa_handler = interrupt;
	
	if (sigaction(SIGINT, &sa, 0) == -1
		|| sigaction(SIGHUP, &sa, 0) == -1) {
		cerr << "Failed to handle SIGINT or SIGHUP: " << strerror(errno) << endl;
		exit(1);
	}
	
	char buf[1024] = {};
	struct sockaddr_un claddr = {};
	socklen_t cllen = sizeof(claddr);
	while (recvfrom(Main_socket, buf, sizeof(buf) - 1, 0,
		(struct sockaddr *)&claddr, &cllen) >= 0) {
// 	while (read(Main_socket, buf, 1023) >= 0) {
		cout << "Processing request [" << buf << "]\n";
		cout << "Sender: " << claddr.sun_path << endl;
		string cmd;
		string req = buf;
		if (req == "EXIT") {
			send_resp(&claddr, "OK");
			cerr << "Exiting upon request" << endl;
			break;
		}
		try {
			size_t pos = req.find(' ');
			size_t pos2, pos3;
			cmd = find_command(req.substr(0, pos));
			if ((pos2 = cmd.find("<args>")) != string::npos) {
				if (pos != string::npos) {
					cmd.replace(pos2, 6, req.substr(pos + 1));
				} else {
					cmd.erase(pos2, 6);
				}
			}
			
			if (cmd.size()) {
				try {
					run_command(cmd);
					send_resp(&claddr, "OK");
				} catch (exception &e) {
					cerr << "Failed to run command: " << e.what() << endl;
					send_resp(&claddr, "CMD FAILED");
				}
			} else {
				// a noop. that's a pass.
				send_resp(&claddr, "OK");
			}
		} catch (exception &e) {
			cerr << "Command not found: " << e.what() << endl;
			send_resp(&claddr, "NOT FOUND");
		}
		memset(buf, '\0', sizeof(buf));
		memset(&claddr, 0, sizeof(claddr));
	}
	
	return 0;
}