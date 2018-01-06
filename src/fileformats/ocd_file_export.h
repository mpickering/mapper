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

#include <QtGlobal>
#include <QByteArray>
#include <QCoreApplication>
#include <QLocale>
#include <QString>
#include <QTextCodec>

#include "fileformats/file_import_export.h"

class QIODevice;

namespace OpenOrienteering {

class Map;
class MapView;
class PointSymbol;
class Symbol;


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
	void initEncoding();
	
	void exportImplementationLegacy();
	
	template< class Format >
	void exportImplementation(quint16 actual_version = Format::version);
	
	template< class File >
	void exportGeoreferencing(File& file);
	
	template< class File >
	void exportColors(File& file);
	
	template< class OcdBaseSymbol >
	void setupBaseSymbol(OcdBaseSymbol& ocd_base_symbol, const Symbol* symbol);
	
	template< class OcdPointSymbol >
	QByteArray exportPointSymbol(const PointSymbol* point_symbol);
	
	template< class File >
	void exportSymbols(File& file);
	
private:
	/// The locale is used for number formatting.
	QLocale locale;
	
	/// Character encoding to use for 1-byte (narrow) strings
	QTextCodec *custom_8bit_encoding = nullptr;
	
};


}  // namespace OpenOrienteering

#endif
