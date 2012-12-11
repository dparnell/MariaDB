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
    MYSQL_RES* result;
    MYSQL_FIELD *fields;
    
    NSNumberFormatter* number_formatter;
    NSDateFormatter* date_time_formatter;
    NSDateFormatter* date_formatter;
    NSDateFormatter* time_formatter;
    int field_count;
    NSArray* field_names;    
}

static void createError(NSError** error, MYSQL* mysql) {
    if(error) {
        int code = mysql_errno(mysql);
        if(code) {
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
    
    MYSQL_RES* res = mysql_store_result(mysql);
    if(res) {
        // ignore any result returned by the query
        mysql_free_result(res);
    }
    
    return YES;
}

- (UInt64)affectedRows {
    return mysql_affected_rows(mysql);
}

- (BOOL)query:(NSString*)sql withCallback:(BOOL(^)(NSDictionary*, NSArray*))callback andError:(NSError**)error {
    if ([self query: sql withError: error]) {
        BOOL flag = YES;
        
        NSDictionary* row;
        while((row = [self nextRow])) {
            if(flag) {
                flag = callback(row, field_names);
            }
        }
        
        return YES;
    }
    return NO;
}

- (NSArray*) query:(NSString*) sql withError:(NSError**)error {
    const char* query = [sql cStringUsingEncoding: NSUTF8StringEncoding];
    
    if (mysql_real_query(mysql, query, (uint)strlen(query))) {
        
        createError(error, mysql);
        return NO;
    }
    
    result = mysql_use_result(mysql);
    field_count = mysql_num_fields(result);
    NSMutableArray* names = [NSMutableArray arrayWithCapacity: field_count];
    fields = mysql_fetch_fields(result);
    
    for(int i=0; i<field_count; i++) {
        [names addObject: [NSString stringWithCString: fields[i].name encoding: NSUTF8StringEncoding]];
    }
    
    field_names = names;
    
    return names;
}

- (NSDictionary*) nextRow {
    MYSQL_ROW row;
    if ((row = mysql_fetch_row(result))) {
        NSMutableDictionary* dict = [NSMutableDictionary dictionaryWithCapacity: field_count];
        unsigned long *lengths = mysql_fetch_lengths(result);
        NSString* str;
        
        for(int i=0; i<field_count; i++) {
            id value;
            
            if(row[i]) {
                if(IS_NUM(fields[i].type)) {
                    if(number_formatter == nil) {
                        number_formatter = [NSNumberFormatter new];
                    }
                    
                    str = [NSString stringWithCString: row[i] encoding: NSUTF8StringEncoding];
                    value = [number_formatter numberFromString: str];
                    if(value == nil) {
                        value = [NSError errorWithDomain: @"Invalid number" code: 0 userInfo: [NSDictionary dictionaryWithObject: str forKey: @"number"]];
                    }
                    
                } else if(fields[i].type == MYSQL_TYPE_BLOB || fields[i].type == MYSQL_TYPE_LONG_BLOB){
                    value = [NSData dataWithBytes: row[i] length: lengths[i]];
                } else if(fields[i].type == MYSQL_TYPE_DATE) {
                    if(date_formatter == nil) {
                        date_formatter = [NSDateFormatter new];
                        [date_formatter setDateFormat: @"yyyy-MM-dd"];
                    }
                    
                    str = [NSString stringWithCString: row[i] encoding: NSUTF8StringEncoding];
                    value = [date_formatter dateFromString: str];
                    if(value == nil) {
                        value = [NSError errorWithDomain: @"Invalid date" code: 0 userInfo: [NSDictionary dictionaryWithObject: str forKey: @"date"]];
                    }
                } else if(fields[i].type == MYSQL_TYPE_TIME) {
                    if(time_formatter == nil) {
                        time_formatter = [NSDateFormatter new];
                        [time_formatter setDateFormat: @"HH:mm:SS"];
                    }
                    str = [NSString stringWithCString: row[i] encoding: NSUTF8StringEncoding];
                    value = [time_formatter dateFromString: str];
                    if(value == nil) {
                        value = [NSError errorWithDomain: @"Invalid time" code: 0 userInfo: [NSDictionary dictionaryWithObject: str forKey: @"time"]];
                    }
                    
                } else if(fields[i].type == MYSQL_TYPE_DATETIME || fields[i].type == MYSQL_TYPE_TIMESTAMP) {
                    if(date_time_formatter == nil) {
                        date_time_formatter = [NSDateFormatter new];
                        [date_time_formatter setDateFormat: @"yyyy-MM-dd HH:mm:SS"];
                    }
                    
                    str = [NSString stringWithCString: row[i] encoding: NSUTF8StringEncoding];
                    value = [date_time_formatter dateFromString: str];
                    if(value == nil) {
                        value = [NSError errorWithDomain: @"Invalid datetime" code: 0 userInfo: [NSDictionary dictionaryWithObject: str forKey: @"datetime"]];
                    }
                } else {
                    value = [NSString stringWithCString: row[i] encoding: NSUTF8StringEncoding];
                }
            } else {
                value = [NSNull null];
            }
            
            [dict setObject: value forKey: [field_names objectAtIndex: i]];
        }
        
        return dict;
    } else {
        mysql_free_result(result);
        field_names = nil;
        fields = nil;
        result = nil;
        field_count = 0;
        
        return nil;
    }
    
}

- (BOOL) close:(NSError **)error {
    if(mysql) {
        mysql_close(mysql);
        mysql = nil;
    }
    
    return YES;
}

- (NSString*) escapeString:(NSString*)aString {
    NSUInteger L = [aString length];
    char* buffer = malloc(L*2);
    mysql_real_escape_string(mysql, buffer, [aString cStringUsingEncoding: NSASCIIStringEncoding], L);
    
    NSString* str = [NSString stringWithCString: buffer encoding: NSASCIIStringEncoding];
    free(buffer);
    return str;
}

- (NSString*) escapeData:(NSData*)someData {
    NSUInteger L = [someData length];
    char* buffer = malloc(L*2);
    mysql_real_escape_string(mysql, buffer, [someData bytes], L);
    
    NSString* str = [NSString stringWithCString: buffer encoding: NSASCIIStringEncoding];
    free(buffer);
    return str;
}

@end
