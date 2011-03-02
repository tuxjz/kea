// Copyright (C) 2011  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>             // for some IPC/network system calls

#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>

#include <config.h>

#include <log/dummylog.h>

#include <dns/question.h>
#include <dns/message.h>
#include <dns/opcode.h>

#include <resolve/resolve.h>
#include <cache/resolver_cache.h>
#include <nsas/address_request_callback.h>
#include <nsas/nameserver_address.h>

#include <asio.hpp>
#include <asiolink/dns_service.h>
#include <asiolink/io_fetch.h>
#include <asiolink/io_service.h>
#include <asiolink/recursive_query.h>

using isc::log::dlog;
using namespace isc::dns;

namespace asiolink {

typedef std::vector<std::pair<std::string, uint16_t> > AddressVector;

// Here we do not use the typedef above, as the SunStudio compiler
// mishandles this in its name mangling, and wouldn't compile.
// We can probably use a typedef, but need to move it to a central
// location and use it consistently.
RecursiveQuery::RecursiveQuery(DNSService& dns_service,
    isc::nsas::NameserverAddressStore& nsas,
    isc::cache::ResolverCache& cache,
    const std::vector<std::pair<std::string, uint16_t> >& upstream,
    const std::vector<std::pair<std::string, uint16_t> >& upstream_root,
    int query_timeout, int client_timeout, int lookup_timeout,
    unsigned retries) :
    dns_service_(dns_service),
    nsas_(nsas), cache_(cache),
    upstream_(new AddressVector(upstream)),
    upstream_root_(new AddressVector(upstream_root)),
    query_timeout_(query_timeout), client_timeout_(client_timeout),
    lookup_timeout_(lookup_timeout), retries_(retries)
{
}

namespace {

typedef std::pair<std::string, uint16_t> addr_t;

/*
 * This is a query in progress. When a new query is made, this one holds
 * the context information about it, like how many times we are allowed
 * to retry on failure, what to do when we succeed, etc.
 *
 * Used by RecursiveQuery::sendQuery.
 */
class RunningQuery : public IOFetch::Callback {

class ResolverNSASCallback : public isc::nsas::AddressRequestCallback {
public:
    ResolverNSASCallback(RunningQuery* rq) : rq_(rq) {}
    
    void success(const isc::nsas::NameserverAddress& address) {
        dlog("Found a nameserver, sending query to " + address.getAddress().toText());
        rq_->nsasCallbackCalled();
        rq_->sendTo(address);
    }
    
    void unreachable() {
        dlog("Nameservers unreachable");
        // Drop query or send servfail?
        rq_->nsasCallbackCalled();
        rq_->makeSERVFAIL();
        rq_->stop(false);
    }

private:
    RunningQuery* rq_;
};


private:
    // The io service to handle async calls
    IOService& io_;

    // Info for (re)sending the query (the question and destination)
    Question question_;

    // This is where we build and store our final answer
    MessagePtr answer_message_;

    // currently we use upstream as the current list of NS records
    // we should differentiate between forwarding and resolving
    boost::shared_ptr<AddressVector> upstream_;

    // Buffer to store the result.
    OutputBufferPtr buffer_;

    // Server to notify when we succeed or fail
    //shared_ptr<DNSServer> server_;
    isc::resolve::ResolverInterface::CallbackPtr resolvercallback_;

    // To prevent both unreasonably long cname chains and cname loops,
    // we simply keep a counter of the number of CNAMEs we have
    // followed so far (and error if it exceeds RESOLVER_MAX_CNAME_CHAIN
    // from lib/resolve/response_classifier.h)
    unsigned cname_count_;

    /*
     * TODO Do something more clever with timeouts. In the long term, some
     *     computation of average RTT, increase with each retry, etc.
     */
    // Timeout information
    int query_timeout_;
    unsigned retries_;

    // normal query state

    // Update the question that will be sent to the server
    void setQuestion(const Question& new_question) {
        question_ = new_question;
    }

    // TODO: replace by our wrapper
    asio::deadline_timer client_timer;
    bool client_timer_canceled_;
    asio::deadline_timer lookup_timer;

    size_t queries_out_;
    
    // If we timed out ourselves (lookup timeout), stop issuing queries
    bool done_;

    // If we have a client timeout, we send back an answer, but don't
    // stop. We use this variable to make sure we don't send another
    // answer if we do find one later (or if we have a lookup_timeout)
    bool answer_sent_;

    // Reference to our NSAS
    isc::nsas::NameserverAddressStore& nsas_;

    // Reference to our cache
    isc::cache::ResolverCache& cache_;
    
    // the 'current' nameserver we have a query out to
    std::string cur_zone_;
    boost::shared_ptr<ResolverNSASCallback> nsas_callback_;
    // this is set to true if we have asked the nsas to give us
    // an address and we are waiting for it to call us back
    bool nsas_callback_out_;
    isc::nsas::NameserverAddress current_ns_address;
    struct timeval current_ns_qsent_time;

    // perform a single lookup; first we check the cache to see
    // if we have a response for our query stored already. if
    // so, call handlerecursiveresponse(), if not, we call send()
    void doLookup() {
        dlog("doLookup: try cache");
        Message cached_message(Message::RENDER);
        isc::resolve::initResponseMessage(question_, cached_message);
        if (cache_.lookup(question_.getName(), question_.getType(),
                          question_.getClass(), cached_message)) {
            dlog("Message found in cache, returning that");
            if (handleRecursiveAnswer(cached_message)) {
                stop(true);
            }
        } else {
            cur_zone_ = ".";
            send();
        }
        
    }

    void sendTo(const isc::nsas::NameserverAddress& address) {
        // We need to keep track of the Address, so that we can update
        // the RTT
        current_ns_address = address;
        gettimeofday(&current_ns_qsent_time, NULL);
        ++queries_out_;
        IOFetch query(IPPROTO_UDP, io_, question_,
            current_ns_address.getAddress(),
            53, buffer_, this,
            query_timeout_);
        io_.get_io_service().post(query);
    }
    
    void send() {
        // If are in forwarder mode, send it to a random
        // forwarder. If not, ask the NSAS for an address
        const int uc = upstream_->size();
        if (uc > 0) {
            int serverIndex = rand() % uc;
            dlog("Sending upstream query (" + question_.toText() +
                ") to " + upstream_->at(serverIndex).first);
            ++queries_out_;
            IOFetch query(IPPROTO_UDP, io_, question_,
                upstream_->at(serverIndex).first,
                upstream_->at(serverIndex).second, buffer_, this,
                query_timeout_);
            io_.get_io_service().post(query);
        } else {
            // Ask the NSAS for an address for the current zone,
            // the callback will call the actual sendTo()
            dlog("Look up nameserver for " + cur_zone_ + " in NSAS");
            // Can we have multiple calls to nsas_out? Let's assume not
            // for now
            std::cout << "[XX] NSASLOOKUP " << this << " for " << cur_zone_ << std::endl;
            assert(!nsas_callback_out_);
            nsas_callback_out_ = true;
            nsas_.lookup(cur_zone_, question_.getClass(), nsas_callback_);
        }
    }
    
    void nsasCallbackCalled() {
        std::cout << "[XX] NSASLOOKUP DONE " << this << " for " << cur_zone_ << std::endl;
        nsas_callback_out_ = false;
    }

    // This function is called by operator() if there is an actual
    // answer from a server and we are in recursive mode
    // depending on the contents, we go on recursing or return
    //
    // Note that the footprint may change as this function may
    // need to append data to the answer we are building later.
    //
    // returns true if we are done (either we have an answer or an
    //              error message)
    // returns false if we are not done
    bool handleRecursiveAnswer(const Message& incoming) {
        dlog("Handle response");
        // In case we get a CNAME, we store the target
        // here (classify() will set it when it walks through
        // the cname chain to verify it).
        Name cname_target(question_.getName());
        
        isc::resolve::ResponseClassifier::Category category =
            isc::resolve::ResponseClassifier::classify(
                question_, incoming, cname_target, cname_count_, true);

        bool found_ns_address = false;
            
        // If the packet is OK, store it in the cache
        //if (!isc::resolve::ResponseClassifier::error(category)) {
        //    cache_.update(incoming);
        //}

        switch (category) {
        case isc::resolve::ResponseClassifier::ANSWER:
        case isc::resolve::ResponseClassifier::ANSWERCNAME:
            // Done. copy and return.
            dlog("Response is an answer");
            cache_.update(incoming);
            isc::resolve::copyResponseMessage(incoming, answer_message_);
            return true;
            break;
        case isc::resolve::ResponseClassifier::CNAME:
            dlog("Response is CNAME!");
            cache_.update(incoming);
            // (unfinished) CNAME. We set our question_ to the CNAME
            // target, then start over at the beginning (for now, that
            // is, we reset our 'current servers' to the root servers).
            if (cname_count_ >= RESOLVER_MAX_CNAME_CHAIN) {
                // just give up
                dlog("CNAME chain too long");
                makeSERVFAIL();
                return true;
            }

            answer_message_->appendSection(Message::SECTION_ANSWER,
                                           incoming);

            question_ = Question(cname_target, question_.getClass(),
                                 question_.getType());

            dlog("Following CNAME chain to " + question_.toText());
            doLookup();
            return false;
            break;
        case isc::resolve::ResponseClassifier::NXDOMAIN:
        case isc::resolve::ResponseClassifier::NXRRSET:
            dlog("Response is NXDOMAIN or NXRRSET");
            // NXDOMAIN, just copy and return.
            // no negcache yet
            //cache_.update(incoming);
            dlog(incoming.toText());
            isc::resolve::copyResponseMessage(incoming, answer_message_);
            return true;
            break;
        case isc::resolve::ResponseClassifier::REFERRAL:
            dlog("Response is referral");
            cache_.update(incoming);
            // Referral. For now we just take the first glue address
            // we find and continue with that

            // auth section should have at least one RRset
            // and one of them should be an NS (otherwise
            // classifier should have error'd)
            // TODO: should we check if it really is subzone?
            for (RRsetIterator rrsi = incoming.beginSection(Message::SECTION_AUTHORITY);
                 rrsi != incoming.endSection(Message::SECTION_AUTHORITY) && !found_ns_address;
                 ++rrsi) {
                ConstRRsetPtr rrs = *rrsi;
                if (rrs->getType() == RRType::NS()) {
                    cur_zone_ = rrs->getName().toText();
                    dlog("Referred to zone " + cur_zone_);
                    found_ns_address = true;
                    break;
                }
            }

            if (found_ns_address) {
                // next resolver round
                // we do NOT use doLookup() here, but send() (i.e. we
                // skip the cache), since if we had the final answer
                // instead of a delegation cached, we would have been
                // there by now.
                send();
                return false;
            } else {
                dlog("No NS RRset in referral?");
                // TODO this will result in answering with the delegation. oh well
                isc::resolve::copyResponseMessage(incoming, answer_message_);
                return true;
            }
            break;
        case isc::resolve::ResponseClassifier::EMPTY:
        case isc::resolve::ResponseClassifier::EXTRADATA:
        case isc::resolve::ResponseClassifier::INVNAMCLASS:
        case isc::resolve::ResponseClassifier::INVTYPE:
        case isc::resolve::ResponseClassifier::MISMATQUEST:
        case isc::resolve::ResponseClassifier::MULTICLASS:
        case isc::resolve::ResponseClassifier::NOTONEQUEST:
        case isc::resolve::ResponseClassifier::NOTRESPONSE:
        case isc::resolve::ResponseClassifier::NOTSINGLE:
        case isc::resolve::ResponseClassifier::OPCODE:
        case isc::resolve::ResponseClassifier::RCODE:
        case isc::resolve::ResponseClassifier::TRUNCATED:
            dlog("Error in response, returning SERVFAIL");
            // Should we try a different server rather than SERVFAIL?
            makeSERVFAIL();
            return true;
            break;
        }
        // should not be reached. assert here?
        // (since we do not have a default in the switch above,
        // the compiler should have errored on any missing case
        // statements
        assert(false);
        return true;
    }
    
public:
    RunningQuery(IOService& io,
        const Question& question,
        MessagePtr answer_message,
        boost::shared_ptr<AddressVector> upstream,
        OutputBufferPtr buffer,
        isc::resolve::ResolverInterface::CallbackPtr cb,
        int query_timeout, int client_timeout, int lookup_timeout,
        unsigned retries,
        isc::nsas::NameserverAddressStore& nsas,
        isc::cache::ResolverCache& cache) :
        io_(io),
        question_(question),
        answer_message_(answer_message),
        upstream_(upstream),
        buffer_(buffer),
        resolvercallback_(cb),
        cname_count_(0),
        query_timeout_(query_timeout),
        retries_(retries),
        client_timer(io.get_io_service()),
        client_timer_canceled_(false),
        lookup_timer(io.get_io_service()),
        queries_out_(0),
        done_(false),
        answer_sent_(false),
        nsas_(nsas),
        cache_(cache),
        nsas_callback_(boost::shared_ptr<ResolverNSASCallback>(
                                     new ResolverNSASCallback(this))),
        nsas_callback_out_(false)
    {
        // Setup the timer to stop trying (lookup_timeout)
        if (lookup_timeout >= 0) {
            lookup_timer.expires_from_now(
                boost::posix_time::milliseconds(lookup_timeout));
            lookup_timer.async_wait(boost::bind(&RunningQuery::stop, this, false));
        }
        
        // Setup the timer to send an answer (client_timeout)
        if (client_timeout >= 0) {
            client_timer.expires_from_now(
                boost::posix_time::milliseconds(client_timeout));
            client_timer.async_wait(boost::bind(&RunningQuery::clientTimeout, this));
        }
        
        doLookup();
    }

    virtual void clientTimeout() {
        dlog("[XX] client timer fired");
        // Return a SERVFAIL, but do not stop until
        // we have an answer or timeout ourselves
        if (!answer_sent_) {
            dlog("[XX] answer not sent yet");
            answer_sent_ = true;
            makeSERVFAIL();
            resolvercallback_->success(answer_message_);
        }
        // if we got here because we canceled it in stop(), we
        // need to go back to stop()
        if (client_timer_canceled_) {
            dlog("[XX] fired due to cancellation");
            stop(false);
        }
    }

    virtual void stop(bool resume) {
        dlog("[XX] stop() called");
        // if we cancel our timers, we will still get an event for
        // that, so we cannot delete ourselves just yet (those events
        // would be bound to a deleted object)
        // cancel them one by one, both cancels should get us back
        // here again.
        // same goes if we have an outstanding query (can't delete
        // until that one comes back to us)
        done_ = true;
        dlog("[XX] stop() called1");
        if (!answer_sent_) {
            dlog("[XX] no answer sent yet");
            answer_sent_ = true;

            // There are two types of messages we could store in the
            // cache;
            // 1. answers to our fetches from authoritative servers,
            //    exactly as we receive them, and
            // 2. answers to queries we received from clients, which
            //    have received additional processing (following CNAME
            //    chains, for instance)
            //
            // Doing only the first would mean we would have to re-do
            // processing when we get data from our cache, and doing
            // only the second would miss out on the side-effect of
            // having nameserver data in our cache.
            //
            // So right now we do both. Since the cache (currently)
            // stores Messages on their question section only, this
            // does mean that we overwrite the messages we stored in
            // the previous iteration if we are following a delegation.
            if (resume) {
                cache_.update(*answer_message_);
    
                resolvercallback_->success(answer_message_);
            } else {
                resolvercallback_->failure();
            }
            return;
        }
        dlog("[XX] stop() called2");
        if (lookup_timer.cancel() != 0) {
            dlog("[XX] lookup timer canceled");
            return;
        }
        dlog("[XX] stop() called3");
        if (client_timer.cancel() != 0) {
            dlog("[XX] client timer canceled");
            client_timer_canceled_ = true;
            return;
        } else {
            dlog("[XX] no client timer anymore");
        }
        dlog("[XX] continuing");
        if (queries_out_ > 0) {
            dlog("[XX] still one or more queries out");
            return;
        }
        dlog("[XX] stop() called4");
        if (nsas_callback_out_) {
            nsas_.cancel(cur_zone_, question_.getClass(), nsas_callback_);
            nsas_callback_out_ = false;
        }
        dlog("Recursive query stopped, deleting");
        delete this;
    }

    // This function is used as callback from DNSQuery.
    virtual void operator()(IOFetch::Result result) {
        // XXX is this the place for TCP retry?
        --queries_out_;
        
        if (!done_ && result != IOFetch::TIME_OUT) {
            // we got an answer

            // Update the NSAS with the time it took
            struct timeval cur_time;
            gettimeofday(&cur_time, NULL);
            uint32_t rtt;
            if (cur_time.tv_sec >= current_ns_qsent_time.tv_sec &&
                cur_time.tv_usec > current_ns_qsent_time.tv_usec) {
                rtt = 1000 * (cur_time.tv_sec - current_ns_qsent_time.tv_sec);
                rtt += (cur_time.tv_usec - current_ns_qsent_time.tv_usec) / 1000;
            } else {
                rtt = 1;
            }

            dlog("RTT: " + boost::lexical_cast<std::string>(rtt));
            current_ns_address.updateRTT(rtt);
            
            Message incoming(Message::PARSE);
            InputBuffer ibuf(buffer_->getData(), buffer_->getLength());
            incoming.fromWire(ibuf);

            buffer_->clear();
            if (recursive_mode() &&
                incoming.getRcode() == Rcode::NOERROR()) {
                done_ = handleRecursiveAnswer(incoming);
            } else {
                isc::resolve::copyResponseMessage(incoming, answer_message_);
                done_ = true;
            }
            
            if (done_) {
                stop(true);
            }
        } else if (!done_ && retries_--) {
            // We timed out, but we have some retries, so send again
            dlog("Timeout for " + question_.toText() + " to " + current_ns_address.getAddress().toText() + ", resending query");
            if (recursive_mode()) {
                current_ns_address.updateRTT(isc::nsas::AddressEntry::UNREACHABLE);
            }
            send();
        } else {
            // out of retries, give up for now
            dlog("Timeout for " + question_.toText() + " to " + current_ns_address.getAddress().toText() + ", giving up");
            if (recursive_mode()) {
                current_ns_address.updateRTT(isc::nsas::AddressEntry::UNREACHABLE);
            }
            if (!answer_sent_) {
                makeSERVFAIL();
            }
            stop(!answer_sent_);
        }
    }
    
    // Clear the answer parts of answer_message, and set the rcode
    // to servfail
    void makeSERVFAIL() {
        isc::resolve::makeErrorMessage(answer_message_, Rcode::SERVFAIL());
    }
    
    // Returns true if we are in 'recursive' mode
    // Returns false if we are in 'forwarding' mode
    // (i.e. if we have anything in upstream_)
    bool recursive_mode() const {
        return upstream_->empty();
    }
};

}

void
RecursiveQuery::resolve(const QuestionPtr& question,
    const isc::resolve::ResolverInterface::CallbackPtr callback)
{
    IOService& io = dns_service_.getIOService();

    MessagePtr answer_message(new Message(Message::RENDER));
    isc::resolve::initResponseMessage(*question, *answer_message);

    OutputBufferPtr buffer(new OutputBuffer(0));

    dlog("Asked to resolve: " + question->toText());
    
    dlog("Try out cache first (direct call to resolve)");
    // First try to see if we have something cached in the messagecache
    if (cache_.lookup(question->getName(), question->getType(),
                      question->getClass(), *answer_message) &&
        answer_message->getRRCount(Message::SECTION_ANSWER) > 0) {
        dlog("Message found in cache, returning that");
        // TODO: err, should cache set rcode as well?
        answer_message->setRcode(Rcode::NOERROR());
        callback->success(answer_message);
    } else {
        // Perhaps we only have the one RRset?
        // TODO: can we do this? should we check for specific types only?
        RRsetPtr cached_rrset = cache_.lookup(question->getName(),
                                              question->getType(),
                                              question->getClass());
        if (cached_rrset) {
            dlog("Found single RRset in cache");
            answer_message->addRRset(Message::SECTION_ANSWER,
                                     cached_rrset);
            answer_message->setRcode(Rcode::NOERROR());
            callback->success(answer_message);
        } else {
            dlog("Message not found in cache, starting recursive query");
            // It will delete itself when it is done
            new RunningQuery(io, *question, answer_message, upstream_,
                             buffer, callback, query_timeout_,
                             client_timeout_, lookup_timeout_, retries_,
                             nsas_, cache_);
        }
    }
}

void
RecursiveQuery::resolve(const Question& question,
                        MessagePtr answer_message,
                        OutputBufferPtr buffer,
                        DNSServer* server)
{
    // XXX: eventually we will need to be able to determine whether
    // the message should be sent via TCP or UDP, or sent initially via
    // UDP and then fall back to TCP on failure, but for the moment
    // we're only going to handle UDP.
    IOService& io = dns_service_.getIOService();

    isc::resolve::ResolverInterface::CallbackPtr crs(
        new isc::resolve::ResolverCallbackServer(server));

    // TODO: general 'prepareinitialanswer'
    answer_message->setOpcode(isc::dns::Opcode::QUERY());
    answer_message->addQuestion(question);
    
    dlog("Asked to resolve: " + question.toText());
    
    // First try to see if we have something cached in the messagecache
    dlog("Try out cache first (started by incoming event)");
    if (cache_.lookup(question.getName(), question.getType(),
                      question.getClass(), *answer_message) &&
        answer_message->getRRCount(Message::SECTION_ANSWER) > 0) {
        dlog("Message found in cache, returning that");
        // TODO: err, should cache set rcode as well?
        answer_message->setRcode(Rcode::NOERROR());
        crs->success(answer_message);
    } else {
        // Perhaps we only have the one RRset?
        // TODO: can we do this? should we check for specific types only?
        RRsetPtr cached_rrset = cache_.lookup(question.getName(),
                                              question.getType(),
                                              question.getClass());
        if (cached_rrset) {
            dlog("Found single RRset in cache");
            answer_message->addRRset(Message::SECTION_ANSWER,
                                     cached_rrset);
            answer_message->setRcode(Rcode::NOERROR());
            crs->success(answer_message);
        } else {
            dlog("Message not found in cache, starting recursive query");
            // It will delete itself when it is done
            new RunningQuery(io, question, answer_message, upstream_,
                                 buffer, crs, query_timeout_, client_timeout_,
                                 lookup_timeout_, retries_, nsas_, cache_);
        }
    }
}



} // namespace asiolink
