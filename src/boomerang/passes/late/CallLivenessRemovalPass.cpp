#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "CallLivenessRemovalPass.h"

#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/util/log/Log.h"


CallLivenessRemovalPass::CallLivenessRemovalPass()
    : IPass("CallLivenessRemoval", PassID::CallLivenessRemoval)
{
}


bool CallLivenessRemovalPass::execute(UserProc *proc)
{
    BasicBlock::RTLRIterator rrit;
    StatementList::reverse_iterator srit;

    for (BasicBlock *bb : *proc->getCFG()) {
        CallStatement *c = dynamic_cast<CallStatement *>(bb->getLastStmt(rrit, srit));

        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr) {
            continue;
        }

        c->removeAllLive();
    }

    return true;
}
