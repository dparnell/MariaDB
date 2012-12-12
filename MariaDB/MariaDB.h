//
//  MariaDB.h
//  MariaDB
//
//  Created by Daniel Parnell on 5/12/12.
//  Copyright (c) 2012 Automagic Software Pty Ltd.
/*
 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 2 of the License, or (at your option) any later version.
 
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.
 
 You should have received a copy of the GNU Library General Public
 License along with this library; if not, write to the Free
 Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 MA 02111-1307, USA
 */
//

#import <Foundation/Foundation.h>

typedef enum MariaDBFieldType { MARIADB_TYPE_DECIMAL, MARIADB_TYPE_TINY,
    MARIADB_TYPE_SHORT,  MARIADB_TYPE_LONG,
    MARIADB_TYPE_FLOAT,  MARIADB_TYPE_DOUBLE,
    MARIADB_TYPE_NULL,   MARIADB_TYPE_TIMESTAMP,
    MARIADB_TYPE_LONGLONG,MARIADB_TYPE_INT24,
    MARIADB_TYPE_DATE,   MARIADB_TYPE_TIME,
    MARIADB_TYPE_DATETIME, MARIADB_TYPE_YEAR,
    MARIADB_TYPE_NEWDATE, MARIADB_TYPE_VARCHAR,
    MARIADB_TYPE_BIT,
    MARIADB_TYPE_NEWDECIMAL=246,
    MARIADB_TYPE_ENUM=247,
    MARIADB_TYPE_SET=248,
    MARIADB_TYPE_TINY_BLOB=249,
    MARIADB_TYPE_MEDIUM_BLOB=250,
    MARIADB_TYPE_LONG_BLOB=251,
    MARIADB_TYPE_BLOB=252,
    MARIADB_TYPE_VAR_STRING=253,
    MARIADB_TYPE_STRING=254,
    MARIADB_TYPE_GEOMETRY=255} MariaDBFieldType;

@interface MariaDB : NSObject

+ (MariaDB*) newWithHost:(NSString*)host user:(NSString*)username password:(NSString*)password database:(NSString*)database port:(UInt16)port andError:(NSError**)error;

- (id) initWithHost:(NSString*)host user:(NSString*)username password:(NSString*)password database:(NSString*)database port:(UInt16)port socket:(NSString*)socket flags:(int)flags andError:(NSError**)error;

- (BOOL)execute:(NSString*)query withError:(NSError**)error;

- (UInt64)affectedRows;

- (BOOL)query:(NSString*)sql withCallback:(BOOL(^)(NSDictionary*, NSArray*))callback andError:(NSError**)error;

- (NSArray*) query:(NSString*) sql withError:(NSError**)error;
- (NSArray*) fieldDataTypes;
- (NSDictionary*) nextRow;

- (BOOL)close:(NSError**)error;

- (NSString*) escapeString:(NSString*)aString;
- (NSString*) escapeData:(NSData*)someData;

@end
