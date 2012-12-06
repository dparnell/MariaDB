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

@interface MariaDB : NSObject

+ (MariaDB*) newWithHost:(NSString*)host user:(NSString*)username password:(NSString*)password database:(NSString*)database port:(UInt16)port andError:(NSError**)error;

- (id) initWithHost:(NSString*)host user:(NSString*)username password:(NSString*)password database:(NSString*)database port:(UInt16)port socket:(NSString*)socket flags:(int)flags andError:(NSError**)error;

- (BOOL)execute:(NSString*)query withError:(NSError**)error;

- (UInt64)affectedRows;

- (BOOL)query:(NSString*)sql withCallback:(BOOL(^)(NSDictionary*, NSArray*))callback andError:(NSError**)error;

- (BOOL)close:(NSError**)error;

@end
