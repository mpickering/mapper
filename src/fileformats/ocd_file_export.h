/*
 *    Copyright 2016-2018 Kai Pastor
 *
 *    Some parts taken from file_format_oc*d8{.h,_p.h,cpp} which are
 *    Copyright 2012 Pete Curtis
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef OPENORIENTEERING_OCD_FILE_EXPORT_H
#define OPENORIENTEERING_OCD_FILE_EXPORT_H

#include <functional>
#include <unordered_map>

#include <QtGlobal>
#include <QByteArray>
#include <QCoreApplication>
#include <QLocale>
#include <QString>
#include <QTextCodec>

#include "core/map_coord.h"
#include "fileformats/file_import_export.h"

class QIODevice;

namespace OpenOrienteering {

class Map;
class MapColor;
class MapView;
class PointObject;
class PointSymbol;
class Symbol;

template< class Format > class OcdFile;


/**
 * An exporter for OCD files.
 */
class OcdFileExport : public Exporter
{
	Q_DECLARE_TR_FUNCTIONS(OpenOrienteering::OcdFileExport)
	
	/**
	 * A type for temporaries helping to convert strings to Ocd format.
	 */
	struct ExportableString
	{
		const QString& string;
		const QTextCodec* custom_8bit_encoding;
		
		operator QByteArray() const
		{
			return custom_8bit_encoding ? custom_8bit_encoding->fromUnicode(string) : string.toUtf8();
		}
		
		operator QString() const noexcept
		{
			return string;
		}
		
	};
	
	ExportableString toOcdString(const QString& string) const
	{
		return { string, custom_8bit_encoding };
	}
	
	
	using StringAppender = void (qint32, const QString&);
	
public:
	/// \todo Add proper API
	static int default_version;
	
	
	OcdFileExport(QIODevice* stream, Map *map, MapView *view);
	
	~OcdFileExport() override;
	
	/**
	 * Exports an OCD file.
	 * 
	 * For now, this simply uses the OCAD8FileExport class.
	 */
	void doExport() override;
	
protected:
	
	template< class Encoding >
	QTextCodec* determineEncoding();
	
	
	void exportImplementationLegacy();
	
	template< class Format >
	void exportImplementation(quint16 version = Format::version);
	
	
	MapCoord calculateAreaOffset();
	
	
	template< class Format >
	void exportSetup(OcdFile<Format>& file);
	
	void exportSetup(quint16 version);
	
	
	template< class Format >
	void exportSymbols(OcdFile<Format>& file, quint16 version);
	
	template< class OcdBaseSymbol >
	void setupBaseSymbol(const Symbol* symbol, OcdBaseSymbol& ocd_base_symbol);
	
	template< class OcdPointSymbol >
	QByteArray exportPointSymbol(const PointSymbol* point_symbol, quint16 version);
	
	template< class Element >
	qint16 exportPattern(const PointSymbol* point, QByteArray& byte_array, quint16 version);		// returns the number of written coordinates, including the headers
	
	template< class Element >
	qint16 exportSubPattern(const MapCoordVector& coords, const Symbol* symbol, QByteArray& byte_array, quint16 version);
	
	void exportSymbolIconV6(const Symbol* symbol, quint8 icon_bits[]);
	
	void exportSymbolIconV9(const Symbol* symbol, quint8 icon_bits[]);
	
	
	template< class Format >
	void exportObjects(OcdFile<Format>& file);
	
	template< class OcdObject >
	QByteArray exportPointObject(const PointObject* point, typename OcdObject::IndexEntryType& entry);
	
	
	template< class Format >
	void exportExtras(OcdFile<Format>& file);
	
	void exportExtras(quint16 version);
	
	
	quint16 convertColor(const MapColor* color) const;
	
	qint32 getPointSymbolExtent(const PointSymbol* symbol) const;
	
	quint16 exportCoordinates(const MapCoordVector& coords, const Symbol* symbol, QByteArray& byte_array);
	
	
private:
	/// The locale is used for number formatting.
	QLocale locale;
	
	/// Character encoding to use for 1-byte (narrow) strings
	QTextCodec *custom_8bit_encoding = nullptr;
	
	MapCoord area_offset;
	
	std::function<StringAppender> addParameterString;
	
	std::unordered_map<const Symbol*, quint32> symbol_numbers;
	
	bool uses_registration_color = false;
};


}  // namespace OpenOrienteering

#endif
