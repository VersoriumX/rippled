//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2021 Ripple Labs Inc.

  Permission to use, copy, modify, and/or distribute this software for any
  purpose  with  or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
  MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/tx/impl/NFTokenMint.h>
#include <ripple/app/tx/impl/details/NFTokenUtils.h>
#include <ripple/basics/Expected.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/InnerObjectFormats.h>
#include <ripple/protocol/Rate.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/st.h>
#include <boost/endian/conversion.hpp>
#include <array>

namespace ripple {

NotTEC
NFTokenMint::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureNonFungibleTokensV1))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfNFTokenMintMask)
        return temINVALID_FLAG;

    if (auto const f = ctx.tx[~sfTransferFee]; f > maxTransferFee)
        return temBAD_TRANSFER_FEE;

    // An issuer must only be set if the tx is executed by the minter
    if (auto iss = ctx.tx[~sfIssuer]; iss == ctx.tx[sfAccount])
        return temMALFORMED;

    if (auto uri = ctx.tx[~sfURI])
    {
        if (uri->length() == 0 || uri->length() > maxTokenURILength)
            return temMALFORMED;
    }

    return preflight2(ctx);
}

uint256
NFTokenMint::createTokenID(
    std::uint16_t flags,
    std::uint16_t fee,
    AccountID const& issuer,
    std::uint32_t taxon,
    std::uint32_t tokenSeq)
{
    // An issuer may issue several NFTs with the same taxon; to ensure that NFTs
    // are spread across multiple pages we lightly mix the taxon up by using the
    // sequence (which is not under the issuer's direct control) as the seed for
    // a simple linear congruential generator.  cipheredTaxon() does this work.
    taxon = nft::cipheredTaxon(tokenSeq, taxon);

    // The values are packed inside a 32-byte buffer, so we need to make sure
    // that the endianess is fixed.
    flags = boost::endian::native_to_big(flags);
    fee = boost::endian::native_to_big(fee);
    taxon = boost::endian::native_to_big(taxon);
    tokenSeq = boost::endian::native_to_big(tokenSeq);

    std::array<std::uint8_t, 32> buf{};

    auto ptr = buf.data();

    // This code is awkward but the idea is to pack these values into a single
    // 256-bit value that uniquely identifies this NFT.
    std::memcpy(ptr, &flags, sizeof(flags));
    ptr += sizeof(flags);

    std::memcpy(ptr, &fee, sizeof(fee));
    ptr += sizeof(fee);

    std::memcpy(ptr, issuer.data(), issuer.size());
    ptr += issuer.size();

    std::memcpy(ptr, &taxon, sizeof(taxon));
    ptr += sizeof(taxon);

    std::memcpy(ptr, &tokenSeq, sizeof(tokenSeq));

    return uint256::fromVoid(buf.data());
}

TER
NFTokenMint::preclaim(PreclaimContext const& ctx)
{
    // The issuer of the NFT may or may not be the account executing this
    // transaction. Check that and verify that this is allowed:
    if (auto issuer = ctx.tx[~sfIssuer])
    {
        auto const sle = ctx.view.read(keylet::account(*issuer));

        if (!sle)
            return tecNO_ISSUER;

        if (auto const minter = (*sle)[~sfMinter]; minter != ctx.tx[sfAccount])
            return tecNO_PERMISSION;
    }

    return tesSUCCESS;
}

TER
NFTokenMint::doApply()
{
    auto const issuer = ctx_.tx[~sfIssuer].value_or(account_);

    auto const tokenSeq = [this, &issuer]() -> Expected<std::uint32_t, TER> {
        auto const root = view().peek(keylet::account(issuer));
        if (root == nullptr)
            // Should not happen.  Checked in preclaim.
            return Unexpected(tecNO_ISSUER);

        // Get the unique sequence number for this token:
        std::uint32_t const tokenSeq = (*root)[~sfMintedTokens].value_or(0);
        {
            std::uint32_t const nextTokenSeq = tokenSeq + 1;
            if (nextTokenSeq < tokenSeq)
                return Unexpected(tecMAX_SEQUENCE_REACHED);

            (*root)[sfMintedTokens] = nextTokenSeq;
        }
        ctx_.view().update(root);
        return tokenSeq;
    }();

    if (!tokenSeq.has_value())
        return (tokenSeq.error());

    std::uint32_t const ownerCountBefore =
        view().read(keylet::account(account_))->getFieldU32(sfOwnerCount);

    auto ret = nft::insertToken(
        ctx_.view(),
        account_,
        {*InnerObjectFormats::getInstance().findSOTemplateBySField(
             sfNonFungibleToken),
         sfNonFungibleToken,
         [this, &issuer, &tokenSeq](STObject& object) {
             object.setFieldH256(
                 sfTokenID,
                 createTokenID(
                     static_cast<std::uint16_t>(
                         ctx_.tx.getFlags() & 0x0000FFFF),
                     ctx_.tx[~sfTransferFee].value_or(0),
                     issuer,
                     ctx_.tx[sfTokenTaxon],
                     tokenSeq.value()));

             if (auto const uri = ctx_.tx[~sfURI])
                 object.setFieldVL(sfURI, *uri);
         }});

    if (ret == tesSUCCESS)
    {
        // Only check the reserve if the owner count actually changed.  This
        // allows NFTs to be added to the page (and burn fees) without
        // requiring the reserve to be met each time.  The reserve is
        // only managed when a new NFT page is added.
        if (auto const ownerCountAfter = view()
                                             .read(keylet::account(account_))
                                             ->getFieldU32(sfOwnerCount);
            ownerCountAfter > ownerCountBefore)
        {
            if (auto const reserve =
                    view().fees().accountReserve(ownerCountAfter);
                mPriorBalance < reserve)
                ret = tecINSUFFICIENT_RESERVE;
        }
    }

    return ret;
}

}  // namespace ripple
