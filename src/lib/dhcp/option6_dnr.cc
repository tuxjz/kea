// Copyright (C) 2023-2024 Internet Systems Consortium, Inc. ("ISC")
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <config.h>

#include <dhcp/option6_dnr.h>

using namespace isc::asiolink;
using namespace isc::util;

namespace isc {
namespace dhcp {

Option6Dnr::Option6Dnr(OptionBufferConstIter begin,
                       OptionBufferConstIter end,
                       bool convenient_notation)
    : Option(V6, D6O_V6_DNR), DnrInstance(V6), convenient_notation_(convenient_notation) {
    unpack(begin, end);
}

OptionPtr
Option6Dnr::clone() const {
    return (cloneInternal<Option6Dnr>());
}

void
Option6Dnr::pack(util::OutputBuffer& buf, bool check) const {
    packHeader(buf, check);

    buf.writeUint16(service_priority_);
    buf.writeUint16(adn_length_);
    packAdn(buf);
    if (adn_only_mode_) {
        return;
    }

    buf.writeUint16(addr_length_);
    packAddresses(buf);
    packSvcParams(buf);
}

void
Option6Dnr::packAddresses(util::OutputBuffer& buf) const {
    for (auto const& address : ip_addresses_) {
        if (!address.isV6()) {
            isc_throw(isc::BadValue, getLogPrefix()
                                         << address.toText() << " is not an IPv6 address");
        }

        buf.writeData(&address.toBytes()[0], V6ADDRESS_LEN);
    }
}

void
Option6Dnr::unpack(OptionBufferConstIter begin, OptionBufferConstIter end) {
    if (convenient_notation_) {
        // parse convenient notation
        std::string config_txt = std::string(begin, end);
        parseConfigData(config_txt);
    } else {
        if (std::distance(begin, end) < getMinimalLength()) {
            isc_throw(OutOfRange, getLogPrefix()
                                      << "data truncated to size " << std::distance(begin, end));
        }

        setData(begin, end);

        // First two octets of Option data is Service Priority - this is mandatory field.
        unpackServicePriority(begin);

        // Next come two octets of ADN Length plus the ADN data itself (variable length).
        // This is Opaque Data Tuple so let's use this class to retrieve the ADN data.
        unpackAdn(begin, end);

        if (begin == end) {
            // ADN only mode, other fields are not included.
            return;
        }

        adn_only_mode_ = false;

        unpackAddresses(begin, end);

        // SvcParams (variable length) field is last.
        unpackSvcParams(begin, end);
    }
}

std::string
Option6Dnr::toText(int indent) const {
    std::ostringstream stream;
    std::string in(indent, ' ');  // base indentation
    stream << in << "type=" << type_ << "(V6_DNR), "
           << "len=" << (len() - getHeaderLen()) << ", " << getDnrInstanceAsText();
    return (stream.str());
}

uint16_t
Option6Dnr::len() const {
    return (OPTION6_HDR_LEN + dnrInstanceLen());
}

void
Option6Dnr::unpackAddresses(OptionBufferConstIter& begin, OptionBufferConstIter end) {
    if (std::distance(begin, end) < getAddrLengthSize()) {
        isc_throw(OutOfRange, getLogPrefix() << "after"
                                                " ADN field, there should be at least "
                                                "2 bytes long Addr Length field");
    }

    // Next come two octets of Addr Length.
    addr_length_ = isc::util::readUint16(&(*begin), getAddrLengthSize());
    begin += getAddrLengthSize();
    // It MUST be a multiple of 16.
    if ((addr_length_ % V6ADDRESS_LEN) != 0) {
        isc_throw(OutOfRange, getLogPrefix()
                                  << "Addr Len=" << addr_length_ << " is not divisible by 16");
    }

    // As per draft-ietf-add-dnr 3.1.8:
    // If additional data is supplied (i.e. not ADN only mode),
    // the option includes at least one valid IP address.
    if (addr_length_ == 0) {
        isc_throw(OutOfRange, getLogPrefix()
                                  << "Addr Len=" << addr_length_
                                  << " but it must contain at least one valid IP address");
    }

    // Check if IPv6 Address(es) field is not truncated.
    if (std::distance(begin, end) < addr_length_) {
        isc_throw(OutOfRange, getLogPrefix() << "Addr Len=" << addr_length_
                                             << " but IPv6 address(es) are truncated to len="
                                             << std::distance(begin, end));
    }

    // Let's unpack the ipv6-address(es).
    auto addr_end = begin + addr_length_;
    while (begin != addr_end) {
        try {
            ip_addresses_.push_back(IOAddress::fromBytes(AF_INET6, &(*begin)));
        } catch (const Exception& ex) {
            isc_throw(BadValue, getLogPrefix() << "failed to parse IPv6 address"
                                               << " - " << ex.what());
        }

        begin += V6ADDRESS_LEN;
    }
}

void
Option6Dnr::parseConfigData(const std::string& config_txt) {
    // This parses convenient option config notation.
    // The config to be parsed may contain escaped characters like "\\," or "\\|".
    // Example configs are below (first contains recommended resolvers' IP addresses, and SvcParams;
    // second is an example of ADN-only mode):
    //
    // "name": "v6-dnr",
    // "data": "100, dot1.example.org., 2001:db8::1 2001:db8::2, alpn=dot\\,doq\\,h2\\,h3 port=8530 dohpath=/q{?dns}"
    //
    // "name": "v6-dnr",
    // "data": "200, resolver.example."

    // get tokens using comma separator with double backslash escaping enabled
    std::vector<std::string> tokens = str::tokens(config_txt, std::string(","), true);

    if (tokens.size() < 2) {
        isc_throw(BadValue, getLogPrefix() << "Option config requires at least comma separated "
                                           << "Service Priority and ADN");
    }

    if (tokens.size() > 4) {
        isc_throw(BadValue, getLogPrefix() << "Option config supports maximum 4 comma separated "
                                           << "fields: Service Priority, ADN, resolver IP "
                                           << "address/es and SvcParams");
    }

    // parse Service Priority
    std::string txt_svc_priority = str::trim(tokens[0]);
    try {
        service_priority_ = boost::lexical_cast<uint16_t>(txt_svc_priority);
    } catch (const std::exception& e) {
        isc_throw(BadValue, getLogPrefix() << "Cannot parse uint_16 integer Service priority "
                                           << "from given value: " << txt_svc_priority
                                           << ". Error: " << e.what());
    }

    // parse ADN
    std::string txt_adn = str::trim(tokens[1]);
    try {
        adn_.reset(new isc::dns::Name(txt_adn, true));
    } catch (const std::exception& e) {
        isc_throw(InvalidOptionDnrDomainName, getLogPrefix() << "Cannot parse ADN FQDN "
                                                             << "from given value: " << txt_adn
                                                             << ". Error: " << e.what());
    }

    adn_length_ = adn_->getLength();
    if (adn_length_ == 0) {
        isc_throw(InvalidOptionDnrDomainName, getLogPrefix()
                                                  << "Mandatory Authentication Domain Name fully "
                                                  << "qualified domain-name is missing");
    }

    if (tokens.size() > 2) {
        setAdnOnlyMode(false);

        // parse resolver IP address/es
        std::string txt_addresses = str::trim(tokens[2]);

        // IP addresses are separated with space
        std::vector<std::string> addresses = str::tokens(txt_addresses, std::string(" "));
        for (auto const& txt_addr : addresses) {
            try {
                ip_addresses_.push_back(IOAddress(str::trim(txt_addr)));
            } catch (const Exception& e) {
                isc_throw(BadValue, getLogPrefix() << "Cannot parse IPv6 address "
                                                   << "from given value: " << txt_addr
                                                   << ". Error: " << e.what());
            }
        }

        // As per RFC9463 section 3.1.8:
        // (If ADN-only mode is not used)
        // The option includes at least one valid IP address.
        if (ip_addresses_.empty()) {
            isc_throw(BadValue, getLogPrefix() << "Option config requires at least one valid IP "
                                               << "address.");
        }

        addr_length_ = ip_addresses_.size() * V6ADDRESS_LEN;
    }

    if (tokens.size() == 4) {
        // parse Service Parameters
        std::string txt_svc_params = str::trim(tokens[3]);

        // SvcParamKey=SvcParamValue pairs are separated with space
        std::vector<std::string> svc_params_pairs = str::tokens(txt_svc_params, std::string(" "));
        std::vector<std::string> alpn_ids_tokens;
        OpaqueDataTuple svc_param_val_tuple(OpaqueDataTuple::LENGTH_2_BYTES);
        OutputBuffer out_buf(2);
        for (auto const& svc_param_pair : svc_params_pairs) {
            std::vector<std::string> key_val_tokens = str::tokens(str::trim(svc_param_pair), "=");
            if (key_val_tokens.size() != 2) {
                isc_throw(InvalidOptionDnrSvcParams,
                          getLogPrefix() << "Wrong Svc Params syntax - SvcParamKey=SvcParamValue "
                                         << "pair syntax must be used");
            }

            // SvcParam Key related checks come below.
            std::string svc_param_key = str::trim(key_val_tokens[0]);

            // As per RFC9463 Section 3.1.8:
            // The service parameters do not include "ipv4hint" or "ipv6hint" parameters.
            if (FORBIDDEN_SVC_PARAMS.find(svc_param_key) != FORBIDDEN_SVC_PARAMS.end()) {
                isc_throw(InvalidOptionDnrSvcParams, getLogPrefix()
                                                         << "Wrong Svc Params syntax - key "
                                                         << svc_param_key << " must not be used");
            }

            // Check if SvcParamKey is known in
            // https://www.iana.org/assignments/dns-svcb/dns-svcb.xhtml
            auto svc_params_iterator = SVC_PARAMS.find(svc_param_key);
            if (svc_params_iterator == SVC_PARAMS.end()) {
                isc_throw(InvalidOptionDnrSvcParams,
                          getLogPrefix() << "Wrong Svc Params syntax - key " << svc_param_key
                                         << " not found in SvcParamKeys registry");
            }

            // Check if SvcParamKey usage is supported by DNR DHCP option.
            // Note that SUPPORTED_SVC_PARAMS set may expand in future.
            uint16_t num_svc_param_key = svc_params_iterator->second;
            if (SUPPORTED_SVC_PARAMS.find(num_svc_param_key) == SUPPORTED_SVC_PARAMS.end()) {
                isc_throw(InvalidOptionDnrSvcParams,
                          getLogPrefix() << "Wrong Svc Params syntax - key " << svc_param_key
                                         << " not supported in DNR option SvcParams");
            }

            // As per RFC9460 Section 2.2:
            // SvcParamKeys SHALL appear in increasing numeric order. (...)
            // There are no duplicate SvcParamKeys.
            //
            // We check for duplicates here. Correct ordering is done when option gets packed.
            if (svc_params_map_.find(num_svc_param_key) != svc_params_map_.end()) {
                isc_throw(InvalidOptionDnrSvcParams, getLogPrefix()
                                                         << "Wrong Svc Params syntax - key "
                                                         << svc_param_key << " is duplicated.");
            }

            // SvcParam Val check.
            std::string svc_param_val = str::trim(key_val_tokens[1]);
            if (svc_param_val.empty()) {
                isc_throw(InvalidOptionDnrSvcParams,
                          getLogPrefix() << "Wrong Svc Params syntax - empty SvcParamValue for key "
                                         << svc_param_key);
            }

            svc_param_val_tuple.clear();
            switch (num_svc_param_key) {
            case 1:
                // alpn
                // The wire-format value for "alpn" consists of at least one alpn-id prefixed by its
                // length as a single octet, and these length-value pairs are concatenated to form
                // the SvcParamValue.
                alpn_ids_tokens = str::tokens(svc_param_val, std::string(","));
                for (auto const& alpn_id : alpn_ids_tokens) {
                    // Check if alpn-id is known in
                    // https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml#alpn-protocol-ids
                    if (ALPN_IDS.find(alpn_id) == ALPN_IDS.end()) {
                        isc_throw(InvalidOptionDnrSvcParams,
                                  getLogPrefix() << "Wrong Svc Params syntax - alpn-id " << alpn_id
                                                 << " not found in ALPN-IDs registry");
                    }

                    // Make notice if this is any of http alpn-ids.
                    if (alpn_id.starts_with('h')) {
                        alpn_http_ = true;
                    }

                    OpaqueDataTuple alpn_id_tuple(OpaqueDataTuple::LENGTH_1_BYTE);
                    alpn_id_tuple.append(alpn_id);
                    alpn_id_tuple.pack(out_buf);
                    svc_param_val_tuple.append(static_cast<const char*>(out_buf.getData()),
                                               out_buf.getLength());
                    out_buf.clear();
                }

                svc_params_map_.insert(std::make_pair(num_svc_param_key, svc_param_val_tuple));
                break;
            case 3:
                // port
                // The wire format of the SvcParamValue is the corresponding 2-octet numeric value
                // in network byte order.
                uint16_t port;
                try {
                    port = boost::lexical_cast<uint16_t>(svc_param_val);
                } catch (const std::exception& e) {
                    isc_throw(InvalidOptionDnrSvcParams,
                              getLogPrefix() << "Cannot parse uint_16 integer port nr "
                                             << "from given value: " << svc_param_val
                                             << ". Error: " << e.what());
                }

                out_buf.writeUint16(port);
                svc_param_val_tuple.append(static_cast<const char*>(out_buf.getData()),
                                           out_buf.getLength());
                out_buf.clear();
                svc_params_map_.insert(std::make_pair(num_svc_param_key, svc_param_val_tuple));
                break;
            case 7:
                // dohpath - RFC9461 Section 5
                // single-valued SvcParamKey whose value (in both presentation format and wire
                // format) MUST be a URI Template in relative form ([RFC6570], Section 1.1) encoded
                // in UTF-8 [RFC3629]. If the "alpn" SvcParam indicates support for HTTP,
                // "dohpath" MUST be present. The URI Template MUST contain a "dns" variable,
                // and MUST be chosen such that the result after DoH URI Template expansion
                // (Section 6 of [RFC8484]) is always a valid and functional ":path" value
                // ([RFC9113], Section 8.3.1).

                // Check that "dns" variable is there
                if (svc_param_val.find("{?dns}") == std::string::npos) {
                    isc_throw(InvalidOptionDnrSvcParams,
                              getLogPrefix()
                                  << "Wrong Svc Params syntax - dohpath SvcParamValue URI"
                                  << " Template MUST contain a 'dns' variable.");
                }

                // We hope to have URI containing < 0x80 ASCII chars, however to be sure
                // and to be inline with RFC9461 Section 5, let's encode the dohpath with utf8.
                auto const utf8_encoded = encode::encodeUtf8(svc_param_val);
                svc_param_val_tuple.append(utf8_encoded.begin(), utf8_encoded.size());
                svc_params_map_.insert(std::make_pair(num_svc_param_key, svc_param_val_tuple));
                break;
            }
        }

        // If the "alpn" SvcParam indicates support for HTTP, "dohpath" MUST be present.
        if (alpn_http_ && svc_params_map_.find(7) == svc_params_map_.end()) {
            isc_throw(InvalidOptionDnrSvcParams,
                      getLogPrefix() << "Wrong Svc Params syntax - dohpath SvcParam missing. "
                                     << "When alpn SvcParam indicates "
                                     << "support for HTTP, dohpath must be present.");
        }

        // At this step all given SvcParams should be fine. We can pack everything to data
        // buffer according to RFC9460 Section 2.2.
        //
        // When the list of SvcParams is non-empty, it contains a series of
        // SvcParamKey=SvcParamValue pairs, represented as:
        // - a 2-octet field containing the SvcParamKey as an integer in network byte order.
        // - a 2-octet field containing the length of the SvcParamValue as an integer
        //   between 0 and 65535 in network byte order. (uint16)
        // - an octet string of this length whose contents are the SvcParamValue in a format
        //   determined by the SvcParamKey.
        // (...)
        // SvcParamKeys SHALL appear in increasing numeric order.
        // Note that (...) there are no duplicate SvcParamKeys.

        std::ostringstream stream;
        for (auto const& svc_param_key : SUPPORTED_SVC_PARAMS) {
            auto it = svc_params_map_.find(svc_param_key);
            if (it != svc_params_map_.end()) {
                // Write 2-octet field containing the SvcParamKey as an integer
                // in network byte order.
                out_buf.writeUint16(it->first);
                // Write 2-octet field containing the length of the SvcParamValue
                // and an octet string of this length whose contents are the SvcParamValue.
                // We use OpaqueDataTuple#pack(&buf) here that will write correct len-data
                // tuple to the buffer.
                (it->second).pack(out_buf);
            }
        }

        // Copy SvcParams buffer from OutputBuffer to OptionBuffer.
        const uint8_t* ptr = static_cast<const uint8_t*>(out_buf.getData());
        OptionBuffer temp_buf(ptr, ptr + out_buf.getLength());
        svc_params_buf_ = temp_buf;
        svc_params_length_ = out_buf.getLength();
        out_buf.clear();

        isc_throw(BadValue, getLogPrefix()
                                << "SvcParams: " + txt_svc_params << ", packed hex: "
                                << str::dumpAsHex(svc_params_buf_.data(), svc_params_length_));
    }
}

}  // namespace dhcp
}  // namespace isc
