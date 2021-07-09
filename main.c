#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <locale.h>
#include <wchar.h>

#define BUFFER_SIZE 256
// backtrack needed because what may look like a departure position may be destination position
#define BACKTRACK_SIZE 8
#define ERROR_MESSAGE_SIZE 256
#define DARK_TILE_COLOR 130
#define LIGHT_TILE_COLOR 223
#define WHITE_PIECE_COLOR 250
#define BLACK_PIECE_COLOR 0

#define UNICODE_BLACK_CHESS_PAWN 0x265f
#define UNICODE_SPACE 0x0020

#define EMPTY_ROW { empty, empty, empty, empty, empty, empty, empty, empty }

typedef enum {white, black} playerSide;
typedef enum {pawn=1, knight=2, bishop=3, rook=4, queen=5, king=6} pieceEnum;
typedef enum {empty = 0, blackPawn = -1, blackKnight = -2, blackBishop = -3, blackRook = -4, blackQueen = -5, blackKing = -6, whitePawn = 1, whiteKnight = 2, whiteBishop = 3, whiteRook = 4, whiteQueen = 5, whiteKing = 6} sidedPiece;
typedef enum {aFile = 1, bFile = 2, cFile = 3, dFile = 4, eFile = 5, fFile = 6, gFile = 7, hFile = 8} chessFile;

typedef struct {
    chessFile file;
    int rank;
} position;

typedef struct moveTag {
    position departurePosition;
    pieceEnum piece;
    playerSide side;
    sidedPiece sidedPiece;
    pieceEnum promoteTo;
    position destination;
    bool isCapture;
    bool isCheck;
    bool isCheckmate;
    bool isShortCastling;
    bool isLongCastling;
} move;

typedef struct moveTreeTag {
    move* move;
    int decisionLevel;
    int probability;
    bool isRoot;
    struct moveTreeTag* firstChoice;
    struct moveTreeTag* nextChoice;
    struct moveTreeTag* previousMove;
} moveTree;

move* new_move() {
    move* m = (move*)malloc(sizeof(move));
    m->departurePosition.file = 0;
    m->departurePosition.rank = 0;
    m->destination.file = 0;
    m->destination.rank = 0;
    m->promoteTo = pawn;
    m->isCapture = m->isCheck = m->isCheckmate = m->isShortCastling = m->isLongCastling = 0;
    m->side = black; // fake root node is black in order to switch to white for first move
    return m;
}

moveTree* new_move_tree() {
    moveTree* t = (moveTree*)malloc(sizeof(moveTree));
    t->firstChoice = NULL;
    t->nextChoice = NULL;
    t->previousMove = NULL;
    t->isRoot = t->decisionLevel = t->probability = 0;
    t->move = new_move();
    return t;
}

void append_move(moveTree* previous, moveTree* next) {
    next->previousMove = previous;
    if (!previous->firstChoice) {
        previous->firstChoice = next;
    } else {
        moveTree* lastChoice = previous->firstChoice;
        while (lastChoice->nextChoice != NULL) {
            lastChoice = lastChoice->nextChoice;
        }
        lastChoice->nextChoice = next;
    }
}

typedef struct {
    bool hasError;
    char errorMessage[ERROR_MESSAGE_SIZE];
    char actualCharacter;
    union {
        int integer;
        int digit;
        chessFile file;
        int rank;
        pieceEnum piece;
        char character;
    };
} readResult;

typedef struct {
    bool hasError;
    union {
        char errorMessage[ERROR_MESSAGE_SIZE];
        moveTree* moveTreeRoot;
        move* move;
    };
} parseResult;

typedef enum {
    startState, beginParseMoveState, parseProbabilityState, parseMovePieceState, parseDepartureFileState, parseDepartureRankState,
    parseMoveCaptureState, parseMoveDestinationState, parseMovePromotionState, parseMoveCheckState, parseMoveCheckmateState,
    parseAlgebraicNotation, parseShortCastlingState, parseLongCastlingState,
    finishParseMoveState, parseWhitespaceState, parseIndentationLevelState,
    parseDepartureOrDestinationState, noDepartureState, endState
} parserState;

static const char* stateStrings[] = {
    "start", "start_parse_move", "parse_probability", "parse_move_piece", "parse_departure_file", "parse_departure_rank",
    "parse_capture", "parse_destination", "parse_promotion", "parse_check", "parse_checkmate",
    "start_algebraic_notation_move", "parse_short_castling", "parse_long_castling",
    "end_parse_move", "parse_whitespace", "parse_indentation_level",
    "departure_or_destination", "no_departure", "end"
};


const char* state_to_string(parserState state) {
    // fprintf(stderr, "state_to_string(%d)\n", state);
    return stateStrings[state];
}

typedef struct {
    int initialCharacterCount;
    parserState stateOnFailure;
    int line;
    int column;
} parserAttempt;

typedef struct {
    int top;
    parserAttempt array[2];
} parserAttemptStack;

parserAttempt* pop_attempt(parserAttemptStack* attemptStack) {
    // fprintf(stderr, "pop_attempt() (top=%d)\n", attemptStack->top);
    if (attemptStack->top == -1) {
        return NULL;
    }
    return &attemptStack->array[attemptStack->top--];
}

void push_attempt(parserAttemptStack* attemptStack, parserAttempt attempt) {
    //fprintf(stderr, "push_attempt() (top=%d)\n", attemptStack->top);
    attemptStack->array[++attemptStack->top] = attempt;
}

typedef struct {
    FILE* file;
    int line;
    int column;
    char buffer[BUFFER_SIZE];
    int bufferSize;
    int bufferCursor; // offset from start of buffer that we have already read
    char errorMessage[ERROR_MESSAGE_SIZE];
    int decisionLevel;
    parserState state;
    move* currentMove;
    moveTree* moveTreeTip;
    moveTree* moveTreeRoot;
    int totalCharacterCount;
    parserAttemptStack attemptStack;
} parser;

parseResult make_parse_error(parser* p, char* msg) {
    parseResult res;
    res.hasError = true;
    strncpy(res.errorMessage, msg, ERROR_MESSAGE_SIZE);
    sprintf(res.errorMessage, "parser error: %s at line %d, column %d", msg, p->line, p->column);
    return res;
}

parseResult make_parse_tree_result(moveTree* moveTreeRoot) {
    parseResult res;
    res.hasError = false;
    res.moveTreeRoot = moveTreeRoot;
    return res;
}

parseResult make_parse_move_result(move* m) {
    parseResult res;
    res.hasError = false;
    res.move = m;
    return res;
}

int read_buffer_initial(parser* p) {
    // fprintf(stderr, "read_buffer_initial()\n");
    p->bufferCursor = 0;
    char* buf = fgets(p->buffer, BUFFER_SIZE, p->file);
    if (buf) {
        // printf("got bufer %s\n", buf);
        p->bufferSize = strlen(p->buffer);
        return p->bufferSize;
    } else {
        return 0;
    }
}

int read_buffer(parser* p) {
    char* newBuffer = (char*)malloc(BUFFER_SIZE*sizeof(char));
    if (newBuffer == NULL) {
        fprintf(stderr, "Failed to allocate memory for new buffer.\n");
        exit(1);
    }

    // read to a separate buffer first before copying around to not mess up buffer before we are certain we got more input
    newBuffer = fgets(newBuffer, BUFFER_SIZE-BACKTRACK_SIZE, p->file);
    if (newBuffer == NULL) {
        return 0;
    }

    // fprintf(stderr, "read_buffer()\n");
    // copy last n characters to the beginning of the buffer (memory)
    strncpy(p->buffer, p->buffer + p->bufferSize - BACKTRACK_SIZE, BACKTRACK_SIZE);

    // reset cursor
    p->bufferCursor = BACKTRACK_SIZE;

    // write after the memory section the bytes that were read
    strncpy(p->buffer + BACKTRACK_SIZE, newBuffer, strlen(newBuffer));

    // fprintf(stderr, "new buffer '%s'\n", p->buffer);
    p->bufferSize = BACKTRACK_SIZE + strlen(newBuffer);

    free(newBuffer);

    return p->bufferSize;
    // fprintf(stderr, "got %d bytes\n", num);
}

int read_buffer_if_needed(parser* p) {
    // fprintf(stderr, "read_buffer_if_needed()\n");
    // fprintf(stderr, "buffer cursor %d vs size %d\n", p->bufferCursor, p->bufferSize);
    if (p->bufferCursor == p->bufferSize) {
        return read_buffer(p);
    }
    return -1;
}

readResult read_character(parser* p, char c) {
    readResult res;
    if (read_buffer_if_needed(p) == 0) {
        res.hasError = true;
        sprintf(res.errorMessage, "Unexpected end of file");
        return res;
    }
    if (p->buffer[p->bufferCursor] == c) {
        p->bufferCursor++;
        p->totalCharacterCount++;
        p->column++;
        if (c == '\n') {
            p->column = 1;
            p->line++;
        }
        res.hasError = false;
        res.character = c;
    } else {
        res.hasError = true;
        sprintf(res.errorMessage, "Expected %c, got '%c'", c, p->buffer[p->bufferCursor]);
    }
    return res;
}

readResult read_one_of_characters(parser* p, char* cs) {
    readResult res;
    if (read_buffer_if_needed(p) == 0) {
        res.hasError = true;
        sprintf(res.errorMessage, "Unexpected end of file");
        return res;
    }
    char c = p->buffer[p->bufferCursor];
    // fprintf(stderr, "got character %c\n", c);
    for (int i = 0; i < strlen(cs); ++i) {
        char t = cs[i];
        // fprintf(stderr, "target character %c\n", t);
        if (c == t) {
            p->bufferCursor++;
            p->totalCharacterCount++;
            p->column++;
            if (c == '\n') {
                p->column = 1;
                p->line++;
            }
            res.hasError = false;
            res.character = c;
            // fprintf(stderr, "wanted character\n");
            return res;
        }
    }
    // fprintf(stderr, "unwanted character\n");
    res.hasError = true;
    res.actualCharacter = c;
    // this message should be overriden by more specific error messages
    sprintf(res.errorMessage, "Unexpected character '%c'", res.actualCharacter);
    return res;
}

readResult read_digit(parser* p) {
    // fprintf(stderr, "buffer cursor (C): %d\n", p->bufferCursor);
    readResult res = read_one_of_characters(p, "0123456789");
    if (res.hasError) {
        // fprintf(stderr, "digit error\n");
        sprintf(res.errorMessage, "Expected a digit, got '%c'", res.actualCharacter);
        return res;
    }
    // fprintf(stderr, "digit no error\n");
    res.digit = res.character - '0';
    return res;
}

readResult read_integer(parser* p) {
    readResult res = read_digit(p);
    if (res.hasError) {
        sprintf(res.errorMessage, "Expected a number, got '%c'", res.actualCharacter);
        return res;
    }
    res.integer = res.digit;
    // fprintf(stderr, "initial integer is %d\n", res.integer);
    readResult tempRes = read_digit(p);
    while (!tempRes.hasError) {
        // fprintf(stderr, "new digit %d, now integer is %d\n", tempRes.digit, res.integer);
        res.integer *= 10;
        res.integer += tempRes.digit;
        tempRes = read_digit(p);
    }
    return res;
}

readResult read_chess_file(parser* p) {
    readResult res = read_one_of_characters(p, "abcdefgh");
    if (res.hasError) {
        sprintf(res.errorMessage, "Expected a rank (1-8), got '%c'", res.actualCharacter);
        return res;
    }
    switch (res.character) {
        case 'a':
            res.file = aFile;
            break;
        case 'b':
            res.file = bFile;
            break;
        case 'c':
            res.file = cFile;
            break;
        case 'd':
            res.file = dFile;
            break;
        case 'e':
            res.file = eFile;
            break;
        case 'f':
            res.file = fFile;
            break;
        case 'g':
            res.file = gFile;
            break;
        case 'h':
            res.file = hFile;
            break;
        default:
            fprintf(stderr, "unexpected chess file character when converting to enum: %c\n", res.character);
            exit(1);
    }
    res.file = res.character;
    return res;
}

readResult read_rank(parser* p) {
    readResult res = read_digit(p);
    if (res.hasError) {
        // fprintf(stderr, "rank error\n");
        sprintf(res.errorMessage, "Expected a rank, got '%c'", res.actualCharacter);
        return res;
    } else if (res.digit == 9) {
        sprintf(res.errorMessage, "9 is not a valid rank (1-8)");
        return res;
    }
    // fprintf(stderr, "rank no error\n");
    res.rank = res.digit;
    return res;
}

readResult read_piece(parser* p) {
    readResult res = read_one_of_characters(p, "NBRQK");
    if (res.hasError) {
        sprintf(res.errorMessage, "Expected a piece symbol (N,B,R,Q,K), got '%c'", res.actualCharacter);
        return res;
    }
    switch (res.character) {
        case 'N':
            res.piece = knight;
            break;
        case 'B':
            res.piece = bishop;
            break;
        case 'R':
            res.piece = rook;
            break;
        case 'Q':
            res.piece = queen;
            break;
        case 'K':
            res.piece = king;
            break;
        default:
            fprintf(stderr, "Unexpected symbol %c when matching piece symbol\n", res.character);
            break;
    }
    return res;
}

readResult read_capture(parser* p) {
    return read_character(p, 'x');
}

readResult read_check(parser* p) {
    return read_character(p, '+');
}

readResult read_checkmate(parser* p) {
    return read_character(p, '#');
}

readResult read_whitespace(parser* p) {
    // fprintf(stderr, "read_whitespace()\n");
    return read_one_of_characters(p, " \n");
}

readResult read_tab(parser* p) {
    return read_character(p, '\t');
}

readResult read_promotion_symbol(parser* p) {
    return read_character(p, '=');
}

readResult read_castling_symbol(parser* p) {
    return read_character(p, 'O');
}

readResult read_dash(parser* p) {
    return read_character(p, '-');
}

parser make_parser(FILE* file) {
    parser p;
    p.file = file;
    p.state = startState;
    p.line = 1;
    p.column = 1;
    p.moveTreeRoot = p.moveTreeTip = new_move_tree();
    p.moveTreeTip->isRoot = true;
    p.decisionLevel = 0;
    p.totalCharacterCount = 0;
    p.attemptStack.top = -1;
    return p;
}

parseResult parse_until(parser* p, parserState untilState) {
    // fprintf(stderr, "parse_until()\n");
    int digit;
    int decisionLevel = 0;
    readResult res;
    moveTree* newMove;
    move* currentMove = p->moveTreeTip->move;
    parserAttempt newAttempt;

    if (read_buffer_initial(p) <= 0) {
        return make_parse_error(p, "cannot read file");
    }

    char errorMessage[256];
    sprintf(errorMessage, "");
    bool failed = false;

    while (p->state != untilState) {
        // fprintf(stderr, "State: %s Line: %d Column: %d Buffer offset: %d\n", state_to_string(p->state), p->line, p->column, p->bufferCursor);
        if (strcmp(errorMessage, "") != 0) {
            // got error, see if we need to backtrack
            parserAttempt* att = pop_attempt(&p->attemptStack);
            if (att != NULL) {
                // NOTE: the attempts must not have altered move tree
                int shiftAmount = p->totalCharacterCount - att->initialCharacterCount;
                // fprintf(stderr, "Attempt failed, backtracking to state %s and shifting input by %d\n", state_to_string(att->stateOnFailure), shiftAmount);
                p->state = att->stateOnFailure;
                p->line = att->line;
                p->column = att->column;
                p->bufferCursor -= shiftAmount;
                p->totalCharacterCount -= shiftAmount;
                sprintf(errorMessage, "");
            } else {
                return make_parse_error(p, errorMessage);
            }
        }
        switch (p->state) {
            case startState:
                p->state = beginParseMoveState;
                break;
            case beginParseMoveState:
                newMove = new_move_tree();
                if (!newMove) {
                    sprintf(errorMessage, "failed to allocate memory for new move");
                    continue;
                }
                newMove->decisionLevel = decisionLevel;
                newMove->move->side = currentMove->side == white ? black : white;
                append_move(p->moveTreeTip, newMove);
                p->moveTreeTip = newMove;
                p->state = parseProbabilityState;
                currentMove = p->moveTreeTip->move;
                break;
            case parseProbabilityState:
                res = read_integer(p);
                if (!res.hasError) {
                    p->moveTreeTip->probability = res.integer;
                    res = read_character(p, '%');
                    if (res.hasError) {
                        sprintf(errorMessage, "%s", res.errorMessage);
                        continue;
                    }
                    res = read_whitespace(p);
                    if (res.hasError) {
                        sprintf(errorMessage, "%s", res.errorMessage);
                        continue;
                    }
                } else {
                    p->moveTreeTip->probability = 100;
                }
                p->state = parseAlgebraicNotation;
                break;
            case parseAlgebraicNotation:
                res = read_castling_symbol(p);
                if (!res.hasError) {
                    res = read_dash(p);
                    if (!res.hasError) {
                        p->state = parseShortCastlingState;
                    } else {
                        sprintf(errorMessage, "%s", res.errorMessage);
                        continue;
                    }
                } else {
                    p->state = parseDepartureOrDestinationState;
                }
                break;
            case parseShortCastlingState:
                res = read_castling_symbol(p);
                if (res.hasError) {
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                res = read_dash(p);
                if (!res.hasError) {
                    p->state = parseLongCastlingState;
                } else {
                    currentMove->piece = king;
                    currentMove->sidedPiece = currentMove->side == white ? whiteKing : blackKing;
                    currentMove->isShortCastling = true;
                    p->state = finishParseMoveState;
                }
                break;
            case parseLongCastlingState:
                res = read_castling_symbol(p);
                if (res.hasError) {
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                currentMove->piece = king;
                currentMove->sidedPiece = currentMove->side == white ? whiteKing : blackKing;
                currentMove->isLongCastling = true;
                p->state = finishParseMoveState;
                break;
            case parseDepartureOrDestinationState:
                // fprintf(stderr, "decision point: departure or destination\n");
                newAttempt.line = p->line;
                newAttempt.column = p->column;
                newAttempt.initialCharacterCount = p->totalCharacterCount;
                newAttempt.stateOnFailure = noDepartureState;
                push_attempt(&p->attemptStack, newAttempt);
                p->state = parseDepartureFileState;
                break;
            case noDepartureState:
                currentMove->departurePosition.file = currentMove->departurePosition.rank = 0;
                p->state = parseMoveDestinationState;
                break;
            case parseDepartureFileState:
                res = read_chess_file(p);
                if (!res.hasError) {
                    currentMove->departurePosition.file = res.file;
                }
                p->state = parseDepartureRankState;
                break;
            case parseDepartureRankState:
                res = read_rank(p);
                if (!res.hasError) {
                    currentMove->departurePosition.rank = res.rank;
                }
                p->state = parseMovePieceState;
                break;
            case parseMovePieceState:
                res = read_piece(p);
                if (!res.hasError) {
                    currentMove->piece = res.piece;
                    currentMove->sidedPiece = currentMove->side == white ? res.piece : -res.piece;
                } else {
                    currentMove->piece = pawn;
                    currentMove->sidedPiece = currentMove->side == white ? whitePawn : blackPawn;
                }
                p->state = parseMoveCaptureState;
                break;
            case parseMoveCaptureState:
                res = read_capture(p);
                if (!res.hasError) {
                    currentMove->isCapture = 1;
                }
                p->state = parseMoveDestinationState;
                break;
            case parseMoveDestinationState:
                // fprintf(stderr, "buffer cursor (A): %d\n", p->bufferCursor);
                res = read_chess_file(p);
                if (!res.hasError) {
                    currentMove->destination.file = res.file;
                    // fprintf(stderr, "got file %d\n", res.file);
                } else {
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                // fprintf(stderr, "buffer cursor (B): %d\n", p->bufferCursor);
                res = read_rank(p);
                if (!res.hasError) {
                    currentMove->destination.rank = res.rank;
                } else {
                    // fprintf(stderr, "parsemovedestination error\n");
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                // no need to go back to this anymore
                pop_attempt(&p->attemptStack);
                p->state = parseMovePromotionState;
                break;
            case parseMovePromotionState:
                res = read_promotion_symbol(p);
                if (!res.hasError) {
                    res = read_piece(p);
                    if (!res.hasError) {
                        currentMove->promoteTo = res.piece;
                    } else {
                        sprintf(errorMessage, "Expected type of piece to promote to, got '%c'", res.actualCharacter);
                        continue;
                    }
                }
                p->state = parseMoveCheckState;
                break;
            case parseMoveCheckState:
                res = read_check(p);
                if (!res.hasError) {
                    currentMove->isCheck = true;
                    p->state = finishParseMoveState;
                } else {
                    p->state = parseMoveCheckmateState;
                }
                break;
            case parseMoveCheckmateState:
                res = read_checkmate(p);
                if (!res.hasError) {
                    currentMove->isCheckmate = true;
                }
                p->state = finishParseMoveState;
                break;
            case finishParseMoveState:
                p->state = parseWhitespaceState;
                break;
            case parseWhitespaceState:
                res = read_whitespace(p);
                if (res.hasError) {
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                while (!res.hasError && res.character != '\n') {
                    res = read_whitespace(p);
                }
                if (res.character == '\n') {
                    p->state = parseIndentationLevelState;
                } else { // end of whitespace, read character
                    p->state = beginParseMoveState;
                }
                break;
            case parseIndentationLevelState:
                decisionLevel = 0;
                res = read_tab(p);
                while (!res.hasError) {
                    decisionLevel += 1;
                    res = read_tab(p);
                }
                // fprintf(stderr, "%s\n", res.errorMessage);

                if (strcmp(res.errorMessage, "Unexpected end of file") == 0) {
                    p->state = endState;
                    continue;
                }

                while (p->moveTreeTip != NULL && p->moveTreeTip->decisionLevel != decisionLevel - 1) {
                    p->moveTreeTip = p->moveTreeTip->previousMove;
                    if (p->moveTreeTip != NULL) {
                        currentMove = p->moveTreeTip->move;
                    } else {
                        currentMove = NULL;
                    }
                }
                if (p->moveTreeTip == NULL) {
                    fprintf(stderr, "indentation error. Make sure you use tabs instead of spaces.\n");
                    exit(1);
                }
                p->decisionLevel = decisionLevel;
                p->state = beginParseMoveState;
                break;
            case endState:
                break;
            default:
                fprintf(stderr, "Unexpected parser state\n");
                exit(1);
                // error
                break;
        }
    }
    return make_parse_tree_result(p->moveTreeRoot);
}

parseResult parse_algebraic_notation(char* buffer) {
    FILE* memfile;
    memfile = fmemopen(buffer, strlen(buffer), "r");

    parser p = make_parser(memfile);
    p.state = parseAlgebraicNotation;
    parseResult res = parse_until(&p, finishParseMoveState);
    fclose(memfile);

    if (!res.hasError && p.totalCharacterCount != strlen(buffer)) {
        return make_parse_error(&p, "Invalid characters after move notation.\n");
    }

    if (!res.hasError) {
        return make_parse_move_result(res.moveTreeRoot->move);
    }

    return res;
}

parseResult parse_variants(FILE* file) {
    parser p = make_parser(file);
    return parse_until(&p, endState);
}

void print_position(position pos) {
    switch (pos.file) {
        case aFile:
            wprintf(L"a");
            break;
        case bFile:
            wprintf(L"b");
            break;
        case cFile:
            wprintf(L"c");
            break;
        case dFile:
            wprintf(L"d");
            break;
        case eFile:
            wprintf(L"e");
            break;
        case fFile:
            wprintf(L"f");
            break;
        case gFile:
            wprintf(L"g");
            break;
        case hFile:
            wprintf(L"h");
            break;
    }
    if (pos.rank) {
        wprintf(L"%d", pos.rank);
    }
}

void print_piece(pieceEnum p) {
    switch (p) {
        case pawn:
            break;
        case knight:
            wprintf(L"N");
            break;
        case bishop:
            wprintf(L"B");
            break;
        case rook:
            wprintf(L"R");
            break;
        case queen:
            wprintf(L"Q");
            break;
        case king:
            wprintf(L"K");
            break;
    }
}

void print_algebraic_notation(move* m) {
    if (m->isShortCastling) {
        wprintf(L"O-O");
        return;
    }
    if (m->isLongCastling) {
        wprintf(L"O-O-O");
        return;
    }
    if (m->departurePosition.file || m->departurePosition.rank) {
        print_position(m->departurePosition);
    }
    print_piece(m->piece);
    if (m->isCapture) {
        wprintf(L"x");
    }
    print_position(m->destination);
    if (m->promoteTo != pawn) {
        wprintf(L"=");
        print_piece(m->promoteTo);
    }
    if (m->isCheck) {
        wprintf(L"+");
    }
    if (m->isCheckmate) {
        wprintf(L"#");
    }
}

void print_tree(moveTree* m) {
    if (m->previousMove && m->previousMove->decisionLevel != m->decisionLevel) {
        wprintf(L"\n");
        for (int i = 0; i < m->decisionLevel; ++i) {
            wprintf(L"\t");
        }
    }
    if (m->probability != 0) {
        wprintf(L"%d%% ", m->probability);
    }
    print_algebraic_notation(m->move);
    wprintf(L" ");
    moveTree*c = m->firstChoice;
    while (c != NULL) {
        print_tree(c);
        c = c->nextChoice;
    }
}

/* Returning a random double floating point from 0 to 1. */
double random_probability() {
    return (double)rand() / (double)RAND_MAX;
}

/** Decide which move to use from the movement tree. Selects moves according to their probability weight. */
moveTree* choose_move(moveTree* currentMove) {
    double totalProbabilityWeight = 0;
    moveTree* choice = currentMove->firstChoice;
    while (choice != NULL) {
        totalProbabilityWeight += choice->probability;
        choice = choice->nextChoice;
    }

    double targetWeight = random_probability() * totalProbabilityWeight;
    double currentWeight = 0;
    choice = currentMove->firstChoice;
    while (choice != NULL) {
        currentWeight += choice->probability;
        if (currentWeight > targetWeight) {
            return choice;
        }
        choice = choice->nextChoice;
    }

    return choice;
}

/** compare two moves, disregarding child/sibling/parent choices in the tree, and probabilities */
bool moves_equal(move* m1, move* m2) {
    return m1->departurePosition.rank == m2->departurePosition.rank && m1->departurePosition.file == m2->departurePosition.file && m1->piece == m2->piece && m1->destination.rank == m2->destination.rank && m1->destination.file == m2->destination.file;
}

/** Add move to move tree. */
moveTree* tree_apply_move(moveTree* t, move* newMove) {
    moveTree* c = t->firstChoice;
    while (c != NULL) {
        if (moves_equal(c->move, newMove)) {
            return c;
        }
        c = c->nextChoice;
    }
    return NULL;
}

typedef struct potentialMoveTag {
    int rankBy;
    int fileBy;
    struct potentialMoveTag *nextPotentialMove;
} potentialMove;

/** Add a potential move to the list. */
potentialMove* add_potential_move(potentialMove* list, int rankBy, int fileBy) {
    potentialMove* p = (potentialMove*)malloc(sizeof(potentialMove));
    if (p == NULL) {
        fprintf(stderr, "not enough memory for potential move");
        exit(1);
    }

    p->rankBy = rankBy;
    p->fileBy = fileBy;
    p->nextPotentialMove = list;

    return p;
}

/** Free memory allocated for potential move list. */
void free_potential_move(potentialMove* list) {
    if (list == NULL) {
        return;
    }
    free_potential_move(list->nextPotentialMove);
    free(list);
}

/** Check if a move from departure (rank, file) to destination (rank, file) would require jumping any pieces, irrespective of moving piece. */
bool no_pieces_jumped(sidedPiece board[8][8], int fromRank, int fromFile, int toRank, int toFile) {
    int rankStep = toRank > fromRank ? 1 : toRank < fromRank ? -1 : 0;
    int fileStep = toFile > fromFile ? 1 : toFile < fromFile ? -1 : 0;
    int currentRank = fromRank + rankStep;
    int currentFile = fromFile + fileStep;
    while (currentRank != toRank || currentFile != toFile) {
        if (board[currentRank][currentFile] != empty) {
            return false;
        }
        currentRank = currentRank + rankStep;
        currentFile = currentFile + fileStep;
    }
    return true;
}

/*
 * Apply the move m to the board, returning true if the move was unique and legal.
 *
 * Disambiguates moves such as Re1 that doesn't specify which rook moves to e1.
 * Assumes that the notation already uniquely specifies piece.
 * TODO: fail and warn when move not unique or illegal.
 */
bool board_apply_move(sidedPiece board[8][8], move* m) {
    if (m->isShortCastling) {
        if (m->side == white) {
            board[0][4] = empty;
            board[0][7] = empty;
            board[0][5] = whiteRook;
            board[0][6] = whiteKing;
        } else {
            board[7][4] = empty;
            board[7][7] = empty;
            board[7][5] = blackRook;
            board[7][6] = blackKing;
        }
        return true;
    } else if (m->isLongCastling) {
        if (m->side == white) {
            board[0][4] = empty;
            board[0][0] = empty;
            board[0][3] = whiteRook;
            board[0][2] = whiteKing;
        } else {
            board[7][4] = empty;
            board[7][0] = empty;
            board[7][3] = blackRook;
            board[7][2] = blackKing;
        }
        return true;
    }
    potentialMove* potentialMoveTip = NULL;
    switch (m->piece) {
        case pawn:
            if (m->isCapture) {
                potentialMoveTip = add_potential_move(potentialMoveTip, m->side == white ? 1 : -1, 1);
                potentialMoveTip = add_potential_move(potentialMoveTip, m->side == white ? 1 : -1, -1);
            } else {
                potentialMoveTip = add_potential_move(potentialMoveTip, m->side == white ? 1 : -1, 0);
                potentialMoveTip = add_potential_move(potentialMoveTip, m->side == white ? 2 : -2, 0);
            }
            break;
        case knight:
            potentialMoveTip = add_potential_move(potentialMoveTip, 2, 1);
            potentialMoveTip = add_potential_move(potentialMoveTip, -2, 1);
            potentialMoveTip = add_potential_move(potentialMoveTip, 2, -1);
            potentialMoveTip = add_potential_move(potentialMoveTip, -2, -1);
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, 2);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, 2);
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, -2);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, -2);
            break;
        case bishop:
            for (int i = 1; i < 8; ++i) {
                // right diagonal
                potentialMoveTip = add_potential_move(potentialMoveTip, i, i);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, -i);
                // left diagonal
                potentialMoveTip = add_potential_move(potentialMoveTip, i, -i);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, i);
            }
            break;
        case rook:
            for (int i = 1; i < 8; ++i) {
                // same rank
                potentialMoveTip = add_potential_move(potentialMoveTip, 0, i);
                potentialMoveTip = add_potential_move(potentialMoveTip, 0, -i);
                // same file
                potentialMoveTip = add_potential_move(potentialMoveTip, i, 0);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, 0);
            }
            break;
        case queen:
            for (int i = -7; i < 8; ++i) {
                // same rank
                potentialMoveTip = add_potential_move(potentialMoveTip, 0, i);
                potentialMoveTip = add_potential_move(potentialMoveTip, 0, -i);
                // same file
                potentialMoveTip = add_potential_move(potentialMoveTip, i, 0);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, 0);
                // right diagonal
                potentialMoveTip = add_potential_move(potentialMoveTip, i, i);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, -i);
                // left diagonal
                potentialMoveTip = add_potential_move(potentialMoveTip, i, -i);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, i);
            }
            break;
        case king:
            // same rank
            potentialMoveTip = add_potential_move(potentialMoveTip, 0, 1);
            potentialMoveTip = add_potential_move(potentialMoveTip, 0, -1);
            // same file
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, 0);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, 0);
            // right diagonal
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, 1);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, -1);
            // left diagonal
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, -1);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, 1);
            break;
    }
    int fromRank = 0, fromFile = 0;
    bool found = false;
    while (potentialMoveTip != NULL) {
        fromRank = m->destination.rank-1 - potentialMoveTip->rankBy;
        fromFile = m->destination.file-1 - potentialMoveTip->fileBy;
        // fprintf(stderr, "checking %d %d (piece=%d)\n", fromFile, fromRank, m->piece);
        if (fromRank >= 0 && fromFile >= 0 && fromRank < 8 && fromFile < 8 &&
            (m->departurePosition.rank == 0 || m->departurePosition.rank == fromRank+1) &&
            (m->departurePosition.file == 0 || m->departurePosition.file == fromFile+1)) {
            sidedPiece sp = board[fromRank][fromFile];
            playerSide s = sp > 0 ? white : black;
            pieceEnum p = sp > 0 ? sp : -sp;
            if (s == m->side && p == m->piece) {
                // cannot go to position occupied by same colored piece
                if (board[m->destination.rank-1][m->destination.file-1] * sp <= 0) {
                    if (p == knight || no_pieces_jumped(board, fromRank, fromFile, m->destination.rank-1, m->destination.file-1)) {
                        found = true;
                        break;
                    }
                }
            }
        }
        potentialMoveTip = potentialMoveTip->nextPotentialMove;
    }
    free_potential_move(potentialMoveTip);
    if (!found) {
        fprintf(stderr, "illegal move!\n");
        exit(1);
    }
    // wprintf(L"from rank %d file %d to rank %d file %d\n", fromRank, fromFile, m->destination.rank-1, m->destination.file-1);
    board[fromRank][fromFile] = empty;
    board[m->destination.rank-1][m->destination.file-1] = m->sidedPiece;
    return true;
}

/** Choose random element from an array of pointers. */
void* random_array_choice(void** choices, int numChoices) {
    int choiceNum = (int)floor(random_probability() * numChoices);
    return choices[choiceNum];
}

void print_greeting() {
    char* greetings[4] = {"Let's play chess!", "Good luck, have fun!", "Let's go!", "Let's see if you  know how to play this opening."};
    char* s = (char*)random_array_choice((void**)greetings, sizeof(greetings)/sizeof(char*));
    wprintf(L"%s\n", s);
}

void print_goodbye() {
    char* messages[2] = {"Goodbye!", "See you again soon!"};
    char* s = (char*)random_array_choice((void**)messages, sizeof(messages)/sizeof(char*));
    wprintf(L"%s\n", s);
}

void print_do_not_understand() {
    char* messages[4] = {"Sorry, I did not understand.", "That doesn't look like a move nor a command.", "Sorry, please rephrase.", "Are you sure that's a move (or command)?"};
    char* s = (char*)random_array_choice((void**)messages, sizeof(messages)/sizeof(char*));
    wprintf(L"%s\n", s);
}

typedef struct {
    sidedPiece board[8][8];
} board;

/** Create a new board having the initial chess position. */
board make_board() {
    sidedPiece board_array[8][8] = {
        { whiteRook, whiteKnight, whiteBishop, whiteQueen, whiteKing, whiteBishop, whiteKnight, whiteRook },
        { whitePawn, whitePawn, whitePawn, whitePawn, whitePawn, whitePawn, whitePawn, whitePawn },
        EMPTY_ROW,
        EMPTY_ROW,
        EMPTY_ROW,
        EMPTY_ROW,
        { blackPawn, blackPawn, blackPawn, blackPawn, blackPawn, blackPawn, blackPawn, blackPawn },
        { blackRook, blackKnight, blackBishop, blackQueen, blackKing, blackBishop, blackKnight, blackRook }
    };
    board b;
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            b.board[i][j] = board_array[i][j];
        }
    }
    return b;
}

void print_board(sidedPiece board[8][8]) {
    for (int rank = 7; rank >= 0; rank--) {
        for (int file = 0; file <= 7; file++) {
            sidedPiece sp = board[rank][file];
            pieceEnum p = sp > 0 ? sp : -sp;
            wchar_t unicodePoint = UNICODE_BLACK_CHESS_PAWN - p + 1;
            if (p == 0) {
                unicodePoint = UNICODE_SPACE;
            }
            // printf("%lc", board[rank][file]);
            // wchar_t star = 0x265f;
            // wprintf(L" %lc ", unicodePoint);
            int tileColor = (rank+file) % 2 == 0 ? DARK_TILE_COLOR : LIGHT_TILE_COLOR;
            int pieceColor = sp > 0 ? WHITE_PIECE_COLOR : BLACK_PIECE_COLOR;

            // ansi color coding magic
            wprintf(L"\e[38;5;%dm\e[48;5;%dm %lc ", pieceColor, tileColor, unicodePoint);
        }
        // last rank hidden if not a lot of whitespace
        wprintf(L"\e[0m\n");
    }
    wprintf(L"\n");
}

void play(moveTree* tree, playerSide userSide, bool blindMode) {
    board theBoard = make_board();
    char* buffer = (char*)malloc(BUFFER_SIZE*sizeof(char));
    //print_board(theBoard.board);
    print_greeting();
    setvbuf(stdin, NULL, _IOLBF, -1);
    moveTree* moveTreeTip = tree;
    if (!blindMode) {
        wprintf(L"\n");
        print_board(theBoard.board);
    }
    if (userSide == black) {
        moveTreeTip = moveTreeTip->firstChoice;
    }
    while (moveTreeTip != NULL) {
        // printf("currentMove:\n");
        if (!moveTreeTip->isRoot) {
            board_apply_move(theBoard.board, moveTreeTip->move);
            print_algebraic_notation(moveTreeTip->move);
            wprintf(L"\n");
            if (!blindMode) {
                print_board(theBoard.board);
            }
        }
        if (moveTreeTip->firstChoice == NULL) {
            break;
        }
        // printf("\n");
        while (true) {
            wprintf(L"> ");
            buffer = fgets(buffer, BUFFER_SIZE, stdin);
            if (buffer == NULL || strcmp(buffer, "exit\n") == 0) {
                if (buffer == NULL) {
                    wprintf(L"ctl-d\n");
                }
                print_goodbye();
                exit(0);
            }

            buffer[strcspn(buffer, "\n")] = 0;

            parseResult res = parse_algebraic_notation(buffer);
            if (res.hasError) {
                //fprintf(stderr, "Failed to parse: %s\n", res.errorMessage);
                print_do_not_understand();
                continue;
            }

            // printf("got move:\n");
            // print_algebraic_notation(res.moveTreeRoot);
            moveTree* goToMove = tree_apply_move(moveTreeTip, res.move);
            if (goToMove == NULL) {
                wprintf(L"wrong move! try again:\n");
            } else {
                moveTreeTip = goToMove;
                board_apply_move(theBoard.board, moveTreeTip->move);
                if (!blindMode) {
                    print_board(theBoard.board);
                }
                break;
            }
        }
        moveTreeTip = choose_move(moveTreeTip);
    }
    wprintf(L"Line played correctly. Good job!\n");
    free(buffer);
}

typedef struct {
    char* inputPath;
    playerSide playerSide;
    bool blindMode;
} options;

options init_options() {
    options options;
    options.inputPath = "";
    options.playerSide = white;
    options.blindMode = false;
    return options;
}

options parse_options(int argc, char* argv[]) {
    options options = init_options();
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--black") == 0) {
            options.playerSide = black;
        } else if (strcmp(argv[i], "--white") == 0) {
            options.playerSide = white;
        } else if (strcmp(argv[i], "--blind") == 0) {
            options.blindMode = true;
        } else if (strlen(options.inputPath) == 0) {
            options.inputPath = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Invalid option %s.\n", argv[i]);
        } else {
            fprintf(stderr, "Unexpected multiple arguments.\n");
        }
    }
    return options;
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    srand(time(0));

    if (argc < 2) {
        fprintf(stderr, "No variants input file specified.\nUsage: $ %s INPUT_FILE\n", argv[0]);
        exit(1);
    }

    options options = parse_options(argc, argv);

    FILE* fp = fopen(options.inputPath, "r");
    parseResult res = parse_variants(fp);
    if (res.hasError) {
        fprintf(stderr, "%s", res.errorMessage);
    }
    // print_tree(res.moveTreeRoot->firstChoice);
    play(res.moveTreeRoot, options.playerSide, options.blindMode);
}
