#ifndef BUBBLEFIGHTER_SERVER_APPLICATION_ENTRY_POINTS_H_
#define BUBBLEFIGHTER_SERVER_APPLICATION_ENTRY_POINTS_H_

namespace bf::entry {

int RunGame(int argc, char** argv);
int RunLogin(int argc, char** argv);
int RunMigrations(int argc, wchar_t** argv);
int RunAdmin(int argc, char** argv);
int RunRoomDs(int argc, char** argv);
int RunDsAgent(int argc, char** argv);

}  // namespace bf::entry

#endif  // BUBBLEFIGHTER_SERVER_APPLICATION_ENTRY_POINTS_H_
