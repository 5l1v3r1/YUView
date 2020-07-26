/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut f�r Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*   
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "ParserAVFormat.h"

#include <QElapsedTimer>

#include "common/parserMacros.h"
#include "AVC/ParserAnnexBAVC.h"
#include "HEVC/ParserAnnexBHEVC.h"
#include "Mpeg2/ParserAnnexBMpeg2.h"
#include "Subtitle/ParserSubtitleDVB.h"
#include "Subtitle/ParserSubtitle608.h"

#define PARSERAVCFORMAT_DEBUG_OUTPUT 0
#if PARSERAVCFORMAT_DEBUG_OUTPUT && !NDEBUG
#include <QDebug>
#define DEBUG_AVFORMAT qDebug
#else
#define DEBUG_AVFORMAT(fmt,...) ((void)0)
#endif

ParserAVFormat::ParserAVFormat(QObject *parent) : ParserBase(parent)
{ 
  // Set the start code to look for (0x00 0x00 0x01)
  startCode.append((char)0);
  startCode.append((char)0);
  startCode.append((char)1);
}

QList<QTreeWidgetItem*> ParserAVFormat::getStreamInfo()
{
  // streamInfoAllStreams containse all the info for all streams.
  // The first QStringPairList contains the general info, next all infos for each stream follows

  QList<QTreeWidgetItem*> info;
  if (streamInfoAllStreams.count() == 0)
    return info;
  
  QStringPairList generalInfo = streamInfoAllStreams[0];
  QTreeWidgetItem *general = new QTreeWidgetItem(QStringList() << "General");
  for (QStringPair p : generalInfo)
    new QTreeWidgetItem(general, QStringList() << p.first << p.second);
  info.append(general);

  for (int i=1; i<streamInfoAllStreams.count(); i++)
  {
    QTreeWidgetItem *streamInfo = new QTreeWidgetItem(QStringList() << QString("Stream %1").arg(i-1));
    for (QStringPair p : streamInfoAllStreams[i])
      new QTreeWidgetItem(streamInfo, QStringList() << p.first << p.second);
    info.append(streamInfo);
  }

  return info;
}

QString ParserAVFormat::getShortStreamDescription(int streamIndex) const
{
  if (streamIndex >= shortStreamInfoAllStreams.count())
    return {};
  return shortStreamInfoAllStreams[streamIndex];
}

bool ParserAVFormat::parseExtradata(QByteArray &extradata)
{
  if (extradata.isEmpty())
    return true;

  if (codecID.isAVC())
    return parseExtradata_AVC(extradata);
  else if (codecID.isHEVC())
    return parseExtradata_hevc(extradata);
  else if (codecID.isMpeg2())
    return parseExtradata_mpeg2(extradata);
  else
    return parseExtradata_generic(extradata);
  return true;
}

bool ParserAVFormat::parseMetadata(QStringPairList &metadata)
{
  if (metadata.isEmpty() || packetModel->isNull())
    return true;

  // Log all entries in the metadata list
  TreeItem *metadataRoot = new TreeItem("Metadata", packetModel->getRootItem());
  for (QStringPair p : metadata)
    new TreeItem(p.first, p.second, "", "", metadataRoot);
  return true;
}

bool ParserAVFormat::parseExtradata_generic(QByteArray &extradata)
{
  if (extradata.isEmpty() || packetModel->isNull())
    return true;

  // Log all bytes in the extradata
  TreeItem *extradataRoot = new TreeItem("Extradata", packetModel->getRootItem());
  for (int i = 0; i < extradata.length(); i++)
  {
    int val = (unsigned char)extradata.at(i);
    QString code = QString("%1 (0x%2)").arg(val, 8, 2, QChar('0')).arg(val, 2, 16, QChar('0'));
    new TreeItem(QString("Byte %1").arg(i), val, "b(8)", code, extradataRoot);
  }
  return true;
}

bool ParserAVFormat::parseExtradata_AVC(QByteArray &extradata)
{
  if (extradata.isEmpty() || packetModel->isNull())
    return true;

  if (extradata.at(0) == 1 && extradata.length() >= 7)
  {
    ReaderHelper reader(extradata, packetModel->getRootItem(), "Extradata (Raw AVC NAL units)");
    IGNOREBITS(8); // Ignore the "1" byte which we already found

    // The extradata uses the avcc format (see avc.c in libavformat)
    unsigned int profile, profile_compat, level, reserved_6_one_bits, nal_size_length_minus1, reserved_3_one_bits, number_of_sps;
    READBITS(profile, 8);
    READBITS(profile_compat, 8);
    READBITS(level, 8);
    READBITS(reserved_6_one_bits, 6);
    READBITS(nal_size_length_minus1, 2);
    READBITS(reserved_3_one_bits, 3);
    READBITS(number_of_sps, 5);

    int pos = 6;
    int nalID = 0;
    for (unsigned int i = 0; i < number_of_sps; i++)
    {
      QByteArray size_bytes = extradata.mid(pos, 2);
      ReaderHelper sps_size_reader(size_bytes, reader.getCurrentItemTree(), QString("SPS %1").arg(i));
      unsigned int sps_size;
      if (!sps_size_reader.readBits(16, sps_size, "sps_size"))
        return false;

      TreeItem *subTree = sps_size_reader.getCurrentItemTree();
      QByteArray rawNAL = extradata.mid(pos+2, sps_size);
      auto parseResult = annexBParser->parseAndAddNALUnit(nalID, rawNAL, {}, {}, subTree);
      if (!parseResult.success)
        subTree->setError();
      else if (parseResult.bitrateEntry)
        this->bitrateItemModel->addBitratePoint(this->videoStreamIndex, *parseResult.bitrateEntry);
      nalID++;
      pos += sps_size + 2;
    }

    int nrPPS = extradata.at(pos++);
    for (int i = 0; i < nrPPS; i++)
    {
      QByteArray size_bytes = extradata.mid(pos, 2);
      ReaderHelper pps_size_reader(size_bytes, reader.getCurrentItemTree(), QString("PPS %1").arg(i));
      unsigned int pps_size;
      if (!pps_size_reader.readBits(16, pps_size, "pps_size"))
        return false;

      TreeItem *subTree = pps_size_reader.getCurrentItemTree();
      QByteArray rawNAL = extradata.mid(pos+2, pps_size);
      auto parseResult = annexBParser->parseAndAddNALUnit(nalID, rawNAL, {}, {}, subTree);
      if (!parseResult.success)
        subTree->setError();
      else if (parseResult.bitrateEntry)
        this->bitrateItemModel->addBitratePoint(this->videoStreamIndex, *parseResult.bitrateEntry);
      nalID++;
      pos += pps_size + 2;
    }
  }

  return true;
}

bool ParserAVFormat::parseExtradata_hevc(QByteArray &extradata)
{
  if (extradata.isEmpty() || packetModel->isNull())
    return true;

  if (extradata.at(0) == 1)
  {
    // The extradata is using the hvcC format
    TreeItem *extradataRoot = new TreeItem("Extradata (HEVC hvcC format)", packetModel->getRootItem());
    hvcC h;
    if (!h.parse_hvcC(extradata, extradataRoot, annexBParser, this->bitrateItemModel.data()))
      return false;
  }
  else if (extradata.at(0) == 0)
  {
    // The extradata does just contain the raw HEVC parameter sets (with start codes).
    QByteArray startCode;
    startCode.append((char)0);
    startCode.append((char)0);
    startCode.append((char)1);

    TreeItem *extradataRoot = new TreeItem("Extradata (Raw HEVC NAL units)", packetModel->getRootItem());

    int nalID = 0;
    int nextStartCode = extradata.indexOf(startCode);
    int posInData = nextStartCode + 3;
    while (nextStartCode >= 0)
    {
      nextStartCode = extradata.indexOf(startCode, posInData);
      int length = nextStartCode - posInData;
      QByteArray nalData = (nextStartCode >= 0) ? extradata.mid(posInData, length) : extradata.mid(posInData);
      // Let the hevc annexB parser parse this
      auto parseResult = annexBParser->parseAndAddNALUnit(nalID, nalData, {}, {}, extradataRoot);
      if (!parseResult.success)
        extradataRoot->setError();
      else if (parseResult.bitrateEntry)
        this->bitrateItemModel->addBitratePoint(this->videoStreamIndex, *parseResult.bitrateEntry);
      nalID++;
      posInData = nextStartCode + 3;
    }
  }
  else
    return ReaderHelper::addErrorMessageChildItem("Unsupported extradata format (configurationVersion != 1)", packetModel->getRootItem());
  
  return true;
}

bool ParserAVFormat::parseExtradata_mpeg2(QByteArray &extradata)
{
  if (extradata.isEmpty() || !packetModel->isNull())
    return true;

  if (extradata.at(0) == 0)
  {
    // The extradata does just contain the raw MPEG2 information
    QByteArray startCode;
    startCode.append((char)0);
    startCode.append((char)0);
    startCode.append((char)1);

    TreeItem *extradataRoot = new TreeItem("Extradata (Raw Mpeg2 units)", packetModel->getRootItem());

    int nalID = 0;
    int nextStartCode = extradata.indexOf(startCode);
    int posInData = nextStartCode + 3;
    while (nextStartCode >= 0)
    {
      nextStartCode = extradata.indexOf(startCode, posInData);
      int length = nextStartCode - posInData;
      QByteArray nalData = (nextStartCode >= 0) ? extradata.mid(posInData, length) : extradata.mid(posInData);
      // Let the hevc annexB parser parse this
      auto parseResult = annexBParser->parseAndAddNALUnit(nalID, nalData, {}, {}, extradataRoot);
      if (!parseResult.success)
        extradataRoot->setError();
      else if (parseResult.bitrateEntry)
        this->bitrateItemModel->addBitratePoint(this->videoStreamIndex, *parseResult.bitrateEntry);
      nalID++;
      posInData = nextStartCode + 3;
    }
  }
  else
    return ReaderHelper::addErrorMessageChildItem("Unsupported extradata format (configurationVersion != 1)", packetModel->getRootItem());

  return true;
}

bool ParserAVFormat::parseAVPacket(unsigned int packetID, AVPacketWrapper &packet)
{
  if (packetModel->isNull())
    return true;

  // Use the given tree item. If it is not set, use the nalUnitMode (if active).
  // Create a new TreeItem root for the NAL unit. We don't set data (a name) for this item
  // yet. We want to parse the item and then set a good description.
  QString specificDescription;
  TreeItem *itemTree = new TreeItem(packetModel->getRootItem());

  int posInData = 0;
  QByteArray avpacketData = QByteArray::fromRawData((const char*)(packet.get_data()), packet.get_data_size());

  AVRational timeBase = timeBaseAllStreams[packet.get_stream_index()];

  auto formatTimestamp = [](int64_t timestamp, AVRational timebase) -> QString
  {
    QString str = QString("%1 (").arg(timestamp);
    if (timestamp < 0)
    {
      str += "-";
      timestamp = -timestamp;
    }
      
    int64_t time = std::abs(timestamp) * 1000 / timebase.num / timebase.den;
      
    int64_t hours = time / 1000 / 60 / 60;
    time -= hours * 60 * 60 * 1000;
    qint64 minutes = time / 1000 / 60;
    time -= minutes * 60 * 1000;
    qint64 seconds = time / 1000;
    qint64 milliseconds = time - seconds;

    if (hours > 0)
      str += QString("%1:").arg(hours);
    if (hours > 0 || minutes > 0)
      str += QString("%1:").arg(minutes, 2, 10, QChar('0'));
    str += QString("%1.").arg(seconds, 2, 10, QChar('0'));
    if (milliseconds < 100)
      str += "0";
    if (milliseconds < 10)
      str += "0";
    str += QString("%1)").arg(milliseconds);

    return str;
  };
    
  // Log all the packet info
  new TreeItem("stream_index", packet.get_stream_index(), itemTree);
  new TreeItem("pts", formatTimestamp(packet.get_pts(), timeBase), itemTree);
  new TreeItem("dts", formatTimestamp(packet.get_dts(), timeBase), itemTree);
  new TreeItem("duration", formatTimestamp(packet.get_duration(), timeBase), itemTree);
  new TreeItem("flag_keyframe", packet.get_flag_keyframe(), itemTree);
  new TreeItem("flag_corrupt", packet.get_flag_corrupt(), itemTree);
  new TreeItem("flag_discard", packet.get_flag_discard(), itemTree);
  new TreeItem("data_size", packet.get_data_size(), itemTree);

  itemTree->setStreamIndex(packet.get_stream_index());

  if (packet.getPacketType() == PacketType::VIDEO)
  {
    if (annexBParser)
    {
      // Colloect the types of NALs to create a good name later
      QStringList nalNames;

      int nalID = 0;
      packetDataFormat_t packetFormat = packet.guessDataFormatFromData();
      const int MIN_NAL_SIZE = 3;
      while (posInData + MIN_NAL_SIZE <= avpacketData.length())
      {
        QByteArray firstBytes = avpacketData.mid(posInData, 4);

        QByteArray nalData;
        if (packetFormat == packetFormatRawNAL)
        {
          int offset;
          if (firstBytes.at(1) == (char)0 && firstBytes.at(2) == (char)0 && firstBytes.at(3) == (char)1)
            offset = 4;
          else if (firstBytes.at(0) == (char)0 && firstBytes.at(1) == (char)0 && firstBytes.at(2) == (char)1)
            offset = 3;
          else
            return ReaderHelper::addErrorMessageChildItem("Start code could not be found.", itemTree);

          // Look for the next start code (or the end of the file)
          int nextStartCodePos = avpacketData.indexOf(startCode, posInData + 3);

          if (nextStartCodePos == -1)
          {
            nalData = avpacketData.mid(posInData + offset);
            posInData = avpacketData.length() + 1;
            DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket start code -1 - NAL from %d to %d", posInData + offset, avpacketData.length());
          }
          else
          {
            const int size = nextStartCodePos - posInData - offset;
            nalData = avpacketData.mid(posInData + offset, size);
            posInData += 3 + size;
            DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket start code %d - NAL from %d to %d", nextStartCodePos, posInData + offset, nextStartCodePos);
          }
        }
        else
        {
          int size = (unsigned char)firstBytes.at(3);
          size += (unsigned char)firstBytes.at(2) << 8;
          size += (unsigned char)firstBytes.at(1) << 16;
          size += (unsigned char)firstBytes.at(0) << 24;
          posInData += 4;

          if (size < 0)
            // The int did overflow. This means that the NAL unit is > 2GB in size. This is probably an error
            return ReaderHelper::addErrorMessageChildItem("Invalid size indicator in packet.", itemTree);
          if (posInData + size > avpacketData.length())
            return ReaderHelper::addErrorMessageChildItem("Not enough data in the input array to read NAL unit.", itemTree);

          nalData = avpacketData.mid(posInData, size);
          posInData += size;
          DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket NAL from %d to %d", posInData, posInData + size);
        }

        // Parse the NAL data
        BitratePlotModel::BitrateEntry packetBitrateEntry;
        packetBitrateEntry.dts = packet.get_dts();
        packetBitrateEntry.pts = packet.get_pts();
        packetBitrateEntry.duration = packet.get_duration();
        auto parseResult = annexBParser->parseAndAddNALUnit(nalID, nalData, packetBitrateEntry, {}, itemTree);

        if (!parseResult.success)
          itemTree->setError();
        else if (parseResult.bitrateEntry)
          this->bitrateItemModel->addBitratePoint(packet.get_stream_index(), *parseResult.bitrateEntry);
        if (parseResult.nalTypeName)
          nalNames.append(*parseResult.nalTypeName);
        nalID++;
      }

      // Create a good detailed and compact description of the AVpacket
      if (codecID.isMpeg2())
        specificDescription = " - ";    // In mpeg2 there is no concept of NAL units
      else
        specificDescription = " - NALs:";
      for (QString n : nalNames)
        specificDescription += (" " + n);
    }
    else if (obuParser)
    {
      int obuID = 0;
      // Colloect the types of OBus to create a good name later
      QStringList obuNames;

      const int MIN_OBU_SIZE = 2;
      while (posInData + MIN_OBU_SIZE <= avpacketData.length())
      {
        QString obuTypeName;
        pairUint64 obuStartEndPosFile; // Not used
        try
        {  
          int nrBytesRead = obuParser->parseAndAddOBU(obuID, avpacketData.mid(posInData), itemTree, obuStartEndPosFile, &obuTypeName);
          DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket parsed OBU %d header %d bytes", obuID, nrBytesRead);
          posInData += nrBytesRead;
        }
        catch (...)
        {
          // Catch exceptions and just return
          break;
        }

        if (!obuTypeName.isEmpty())
          obuNames.append(obuTypeName);
        obuID++;

        if (obuID > 200)
        {
          DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket We encountered more than 200 OBUs in one packet. This is probably an error.");
          return false;
        }
      }

      specificDescription = " - OBUs:";
      for (QString n : obuNames)
        specificDescription += (" " + n);
    }
  }
  else if (packet.getPacketType() == PacketType::SUBTITLE_DVB)
  {
    QStringList segmentNames;
    int segmentID = 0;

    const int MIN_DVB_SEGMENT_SIZE = 6;
    while (posInData + MIN_DVB_SEGMENT_SIZE <= avpacketData.length())
    {
      QString segmentTypeName;
      try
      {  
        int nrBytesRead = subtitle_dvb::parseDVBSubtitleSegment(avpacketData.mid(posInData), itemTree, &segmentTypeName);
        DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket parsed DVB segment %d - %d bytes", obuID, nrBytesRead);
        posInData += nrBytesRead;
      }
      catch (...)
      {
        // Catch exceptions and just return
        break;
      }

      if (!segmentTypeName.isEmpty())
        segmentNames.append(segmentTypeName);
      segmentID++;

      if (segmentID > 200)
      {
        DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket We encountered more than 200 DVB segments in one packet. This is probably an error.");
        return false;
      }
    }
  }
  else if (packet.getPacketType() == PacketType::SUBTITLE_608)
  {
    try
    {
      subtitle_608::parse608SubtitlePacket(avpacketData, itemTree);
    }
    catch (...)
    {
      // Catch exceptions
    }
  }
  else
  {
    TreeItem *rawDataRoot = new TreeItem("Data", itemTree);
    const auto nrBytesToLog = std::min(avpacketData.length(), 100);
    for (int i = 0; i < nrBytesToLog; i++)
    {
      int val = (unsigned char)avpacketData.at(i);
      QString code = QString("%1 (0x%2)").arg(val, 8, 2, QChar('0')).arg(val, 2, 16, QChar('0'));
      new TreeItem(QString("Byte %1").arg(i), val, "b(8)", code, rawDataRoot);
    }

    BitratePlotModel::BitrateEntry entry;
    entry.pts = packet.get_pts();
    entry.dts = packet.get_dts();
    entry.duration = packet.get_duration();
    entry.bitrate = packet.get_data_size();
    entry.keyframe = packet.get_flag_keyframe();
    bitrateItemModel->addBitratePoint(packet.get_stream_index(), entry);
  }

  // Set a useful name of the TreeItem (the root for this NAL)
  itemTree->itemData.append(QString("AVPacket %1%2").arg(packetID).arg(packet.get_flag_keyframe() ? " - Keyframe": "") + specificDescription);

  return true;
}

bool ParserAVFormat::hvcC::parse_hvcC(QByteArray &hvcCData, TreeItem *root, QScopedPointer<ParserAnnexB> &annexBParser, BitratePlotModel *bitrateModel)
{
  ReaderHelper reader(hvcCData, root, "hvcC");
  reader.disableEmulationPrevention();

  unsigned int reserved_4onebits, reserved_5onebits, reserver_6onebits;

  // The first 22 bytes are the hvcC header
  READBITS(configurationVersion, 8);
  if (configurationVersion != 1)
    return reader.addErrorMessageChildItem("Only configuration version 1 supported.");
  READBITS(general_profile_space, 2);
  READFLAG(general_tier_flag);
  READBITS(general_profile_idc, 5);
  READBITS(general_profile_compatibility_flags, 32);
  READBITS(general_constraint_indicator_flags, 48);
  READBITS(general_level_idc, 8);
  READBITS(reserved_4onebits, 4);
  if (reserved_4onebits != 15)
    return reader.addErrorMessageChildItem("The reserved 4 one bits should all be one.");
  READBITS(min_spatial_segmentation_idc, 12);
  READBITS(reserver_6onebits, 6);
  if (reserver_6onebits != 63)
    return reader.addErrorMessageChildItem("The reserved 6 one bits should all be one.");
  QStringList parallelismTypeMeaning = QStringList()
    << "mixed-type parallel decoding"
    << "slice-based parallel decoding"
    << "tile-based parallel decoding"
    << "wavefront-based parallel decoding";
  READBITS_M(parallelismType, 2, parallelismTypeMeaning);
  READBITS(reserver_6onebits, 6);
  if (reserver_6onebits != 63)
    return reader.addErrorMessageChildItem("The reserved 6 one bits should all be one.");
  READBITS(chromaFormat, 2);
  READBITS(reserved_5onebits, 5);
  if (reserved_5onebits != 31)
    return reader.addErrorMessageChildItem("The reserved 6 one bits should all be one.");
  READBITS(bitDepthLumaMinus8, 3);
  READBITS(reserved_5onebits, 5);
  if (reserved_5onebits != 31)
    return reader.addErrorMessageChildItem("The reserved 6 one bits should all be one.");
  READBITS(bitDepthChromaMinus8, 3);
  READBITS(avgFrameRate, 16);
  READBITS(constantFrameRate, 2);
  READBITS(numTemporalLayers, 3);
  READFLAG(temporalIdNested);
  READBITS(lengthSizeMinusOne, 2);
  READBITS(numOfArrays, 8);

  // Now parse the contained raw NAL unit arrays
  for (unsigned int i = 0; i < numOfArrays; i++)
  {
    hvcC_naluArray a;
    if (!a.parse_hvcC_naluArray(i, reader, annexBParser, bitrateModel))
      return false;
    naluArrayList.append(a);
  }
  return true;
}

bool ParserAVFormat::hvcC_naluArray::parse_hvcC_naluArray(int arrayID, ReaderHelper &reader, QScopedPointer<ParserAnnexB> &annexBParser, BitratePlotModel *bitrateModel)
{
  ReaderSubLevel sub_level_adder(reader, QString("nal unit array %1").arg(arrayID));

  // The next 3 bytes contain info about the array
  READFLAG(array_completeness);
  READFLAG(reserved_flag_false);
  if (reserved_flag_false)
    return reader.addErrorMessageChildItem("The reserved_flag_false should be false.");
  READBITS(NAL_unit_type, 6);
  READBITS(numNalus, 16);
  
  for (unsigned int i = 0; i < numNalus; i++)
  {
    hvcC_nalUnit nal;
    if (!nal.parse_hvcC_nalUnit(i, reader, annexBParser, bitrateModel))
      return false;
    nalList.append(nal);
  }

  return true;
}

bool ParserAVFormat::hvcC_nalUnit::parse_hvcC_nalUnit(int unitID, ReaderHelper &reader, QScopedPointer<ParserAnnexB> &annexBParser, BitratePlotModel *bitrateModel)
{
  ReaderSubLevel sub_level_adder(reader, QString("nal unit %1").arg(unitID));

  READBITS(nalUnitLength, 16);

  // Get the bytes of the raw nal unit to pass to the "real" hevc parser
  QByteArray nalData = reader.readBytes(nalUnitLength);

  // Let the hevc annexB parser parse this
  auto parseResult = annexBParser->parseAndAddNALUnit(unitID, nalData, {}, {}, reader.getCurrentItemTree());
  if (!parseResult.success)
    return false;
  else if (bitrateModel != nullptr && parseResult.bitrateEntry)
    bitrateModel->addBitratePoint(0, *parseResult.bitrateEntry);

  return true;
}

bool ParserAVFormat::runParsingOfFile(QString compressedFilePath)
{
  // Open the file but don't parse it yet.
  QScopedPointer<fileSourceFFmpegFile> ffmpegFile(new fileSourceFFmpegFile());
  if (!ffmpegFile->openFile(compressedFilePath, nullptr, nullptr, false))
  {
    emit backgroundParsingDone("Error opening the ffmpeg file.");
    return false;
  }

  codecID = ffmpegFile->getVideoStreamCodecID();
  if (codecID.isAVC())
    annexBParser.reset(new ParserAnnexBAVC());
  else if (codecID.isHEVC())
    annexBParser.reset(new ParserAnnexBHEVC());
  else if (codecID.isMpeg2())
    annexBParser.reset(new ParserAnnexBMpeg2());
  else if (codecID.isAV1())
    obuParser.reset(new ParserAV1OBU());
  else if (codecID.isNone())
  {
    emit backgroundParsingDone("Unknown codec ID " + codecID.getCodecName());
    return false;
  }

  int max_ts = ffmpegFile->getMaxTS();
  videoStreamIndex = ffmpegFile->getVideoStreamIndex();

  // Don't seek to the beginning here. This causes more problems then it solves.
  // ffmpegFile->seekFileToBeginning();

  // First get the extradata and push it to the parser
  try
  {
    QByteArray extradata = ffmpegFile->getExtradata();
    parseExtradata(extradata);
  }
  catch (...)
  {
    emit backgroundParsingDone("Error parsing Extradata from container");
    return false;
  }
  try
  {
    QStringPairList metadata = ffmpegFile->getMetadata();
    parseMetadata(metadata);
  }
  catch (...)
  {
    emit backgroundParsingDone("Error parsing Metadata from container");
    return false;
  }

  // After opening the file, we can get information on it
  streamInfoAllStreams = ffmpegFile->getFileInfoForAllStreams();
  timeBaseAllStreams = ffmpegFile->getTimeBaseAllStreams();
  shortStreamInfoAllStreams = ffmpegFile->getShortStreamDescriptionAllStreams();

  emit streamInfoUpdated();

  // Now iterate over all packets and send them to the parser
  AVPacketWrapper packet = ffmpegFile->getNextPacket(false, false);
  int64_t start_ts = packet.get_dts();

  unsigned int packetID = 0;
  unsigned int videoFrameCounter = 0;
  bool abortParsing = false;
  QElapsedTimer signalEmitTimer;
  signalEmitTimer.start();
  while (!ffmpegFile->atEnd() && !abortParsing)
  {
    if (packet.getPacketType() == PacketType::VIDEO)
    {
      if (max_ts != 0)
        progressPercentValue = clip(int((packet.get_dts() - start_ts) * 100 / max_ts), 0, 100);
      videoFrameCounter++;
    }

    if (!parseAVPacket(packetID, packet))
    {
      DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket error parsing Packet %d", packetID);
    }
    else
    {
      DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket Packet %d", packetID);
    }

    packetID++;
    packet = ffmpegFile->getNextPacket(false, false);
    
    // For signal slot debugging purposes, sleep
    // QThread::msleep(200);
    
    if (signalEmitTimer.elapsed() > 1000 && packetModel)
    {
      signalEmitTimer.start();
      emit modelDataUpdated();
    }

    if (cancelBackgroundParser)
    {
      abortParsing = true;
      DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket Abort parsing by user request");
    }
    if (parsingLimitEnabled && videoFrameCounter > PARSER_FILE_FRAME_NR_LIMIT)
    {
      DEBUG_AVFORMAT("ParserAVFormat::parseAVPacket Abort parsing because frame limit was reached.");
      abortParsing = true;
    }
  }

  // Seek back to the beginning of the stream.
  ffmpegFile->seekFileToBeginning();

  if (packetModel)
    emit modelDataUpdated();

  streamInfoAllStreams = ffmpegFile->getFileInfoForAllStreams();
  emit streamInfoUpdated();
  emit backgroundParsingDone("");

  return !cancelBackgroundParser;
}
