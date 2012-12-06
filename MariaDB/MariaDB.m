//
//  MariaDB.m
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

#import "MariaDB.h"

#import "mysql.h"

@implementation MariaDB {
    MYSQL* mysql;
}

static void createError(NSError** error, MYSQL* mysql) {
    if(error) {
        int code = mysql_errno(mysql);
        NSString* str = [NSString stringWithFormat: @"%s", mysql_error(mysql)];
        NSString* state = [NSString stringWithFormat: @"%s", mysql_sqlstate(mysql)];
        NSDictionary* dict = [NSDictionary dictionaryWithObjectsAndKeys:
                              [NSNumber numberWithInt: code], @"Code",
                              state, @"SQL State",
                              str, @"SQL Error",
                              nil];
        NSError* err = [NSError errorWithDomain: str code: code userInfo: dict];
        
        *error = err;
    }
}

+ (MariaDB*) newWithHost:(NSString*)host user:(NSString*)username password:(NSString*)password database:(NSString*)database port:(UInt16)port andError:(NSError**)error {
    return [[MariaDB alloc] initWithHost: host user: username password: password database: database port: port socket: nil flags: 0 andError: error];
}

- (id) initWithHost:(NSString*)host user:(NSString*)username password:(NSString*)password database:(NSString*)database port:(UInt16)port socket:(NSString*)socket flags:(int)flags andError:(NSError **)error {
    self = [super init];
    if(self) {
        mysql = mysql_init(nil);
        if(!mysql_real_connect(mysql,
                               [host cStringUsingEncoding: NSUTF8StringEncoding],
                               [username cStringUsingEncoding: NSUTF8StringEncoding],
                               [password cStringUsingEncoding: NSUTF8StringEncoding],
                               [database cStringUsingEncoding: NSUTF8StringEncoding],
                               port,
                               [socket cStringUsingEncoding: NSUTF8StringEncoding],
                               flags)) {
            
            createError(error, mysql);
            [self close: nil];
            
            return nil;
        }
    }
    
    return self;
}

- (void) dealloc {
    [self close: nil];
}

- (BOOL)execute:(NSString*)query withError:(NSError**)error {
    const char* sql = [query cStringUsingEncoding: NSUTF8StringEncoding];
    
    if (mysql_real_query(mysql, sql, (uint)strlen(sql))) {
        
        createError(error, mysql);
        return NO;
    }
    
    return YES;
}

- (UInt64)affectedRows {
    return mysql_affected_rows(mysql);
}

- (BOOL) close:(NSError **)error {
    if(mysql) {
        mysql_close(mysql);
        mysql = nil;
    }
    
    return YES;
}

@end
