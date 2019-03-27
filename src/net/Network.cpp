/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2018-2019 SChernykh   <https://github.com/SChernykh>
 * Copyright 2016-2019 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _MSC_VER
#pragma warning(disable:4244)
#endif

#include <inttypes.h>
#include <memory>
#include <time.h>


#include "api/Api.h"
#include "base/io/log/Log.h"
#include "base/net/stratum/Client.h"
#include "base/net/stratum/SubmitResult.h"
#include "base/tools/Chrono.h"
#include "base/tools/Timer.h"
#include "core/Config.h"
#include "core/Controller.h"
#include "net/Network.h"
#include "net/strategies/DonateStrategy.h"
#include "workers/Workers.h"


xmrig::Network::Network(Controller *controller) :
    m_donate(nullptr),
    m_timer(nullptr)
{
    Workers::setListener(this);
    controller->addListener(this);

    const Pools &pools = controller->config()->pools();
    m_strategy = pools.createStrategy(this);

    if (pools.donateLevel() > 0) {
        m_donate = new DonateStrategy(controller, this);
    }

    m_timer = new Timer(this, kTickInterval, kTickInterval);
}


xmrig::Network::~Network()
{
    delete m_timer;

    if (m_donate) {
        delete m_donate;
    }

    delete m_strategy;
}


void xmrig::Network::connect()
{
    m_strategy->connect();
}


void xmrig::Network::onActive(IStrategy *strategy, Client *client)
{
    if (m_donate && m_donate == strategy) {
        LOG_NOTICE("dev donate started");
        return;
    }

    m_state.setPool(client->host(), client->port(), client->ip());

    const char *tlsVersion = client->tlsVersion();
    LOG_INFO(WHITE_BOLD("use pool ") CYAN_BOLD("%s:%d ") GREEN_BOLD("%s") " " BLACK_BOLD("%s"),
             client->host(), client->port(), tlsVersion ? tlsVersion : "", client->ip());

    const char *fingerprint = client->tlsFingerprint();
    if (fingerprint != nullptr) {
        LOG_INFO(BLACK_BOLD("fingerprint (SHA-256): \"%s\""), fingerprint);
    }
}


void xmrig::Network::onConfigChanged(Config *config, Config *previousConfig)
{
    if (config->pools() == previousConfig->pools() || !config->pools().active()) {
        return;
    }

    m_strategy->stop();

    config->pools().print();

    delete m_strategy;
    m_strategy = config->pools().createStrategy(this);
    connect();
}


void xmrig::Network::onJob(IStrategy *strategy, Client *client, const Job &job)
{
    if (m_donate && m_donate->isActive() && m_donate != strategy) {
        return;
    }

    setJob(client, job, m_donate == strategy);
}


void xmrig::Network::onJobResult(const JobResult &result)
{
    if (result.poolId == -1 && m_donate) {
        m_donate->submit(result);
        return;
    }

    m_strategy->submit(result);
}


void xmrig::Network::onPause(IStrategy *strategy)
{
    if (m_donate && m_donate == strategy) {
        LOG_NOTICE("dev donate finished");
        m_strategy->resume();
    }

    if (!m_strategy->isActive()) {
        LOG_ERR("no active pools, stop mining");
        m_state.stop();
        return Workers::pause();
    }
}


void xmrig::Network::onResultAccepted(IStrategy *, Client *, const SubmitResult &result, const char *error)
{
    m_state.add(result, error);

    if (error) {
        LOG_INFO(RED_BOLD("rejected") " (%" PRId64 "/%" PRId64 ") diff " WHITE_BOLD("%u") " " RED("\"%s\"") " " BLACK_BOLD("(%" PRIu64 " ms)"),
                 m_state.accepted, m_state.rejected, result.diff, error, result.elapsed);
    }
    else {
        LOG_INFO(GREEN_BOLD("accepted") " (%" PRId64 "/%" PRId64 ") diff " WHITE_BOLD("%u") " " BLACK_BOLD("(%" PRIu64 " ms)"),
                 m_state.accepted, m_state.rejected, result.diff, result.elapsed);
    }
}


void xmrig::Network::setJob(Client *client, const Job &job, bool donate)
{
    if (job.height()) {
        LOG_INFO(MAGENTA_BOLD("new job") " from " WHITE_BOLD("%s:%d") " diff " WHITE_BOLD("%d") " algo " WHITE_BOLD("%s") " height " WHITE_BOLD("%" PRIu64),
                 client->host(), client->port(), job.diff(), job.algorithm().shortName(), job.height());
    }
    else {
        LOG_INFO(MAGENTA_BOLD("new job") " from " WHITE_BOLD("%s:%d") " diff " WHITE_BOLD("%d") " algo " WHITE_BOLD("%s"),
                 client->host(), client->port(), job.diff(), job.algorithm().shortName());
    }

    if (!donate && m_donate) {
        m_donate->setAlgo(job.algorithm());
    }

    m_state.diff = job.diff();
    Workers::setJob(job, donate);
}


void xmrig::Network::tick()
{
    const uint64_t now = Chrono::steadyMSecs();

    m_strategy->tick(now);

    if (m_donate) {
        m_donate->tick(now);
    }

#   ifndef XMRIG_NO_API
    Api::tick(m_state);
#   endif
}