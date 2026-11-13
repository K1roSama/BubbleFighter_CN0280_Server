#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_ADMIN_MODERATION_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_ADMIN_MODERATION_H_

/**
 * @file moderation.h
 * @brief 管理CLI独立于客户端opcode，客户端包不能提供管理授权。
 */
int RunModerationCommand(int argc, char** argv);
bool IsModerationCommand(const char* command);
void PrintModerationUsage();

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_ADMIN_MODERATION_H_
