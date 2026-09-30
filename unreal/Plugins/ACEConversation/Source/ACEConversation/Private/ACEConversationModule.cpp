#include "Modules/ModuleManager.h"
#include "ACEConversationLog.h"

DEFINE_LOG_CATEGORY(LogACEConversation);

class FACEConversationModule : public IModuleInterface
{
};

IMPLEMENT_MODULE(FACEConversationModule, ACEConversation)

